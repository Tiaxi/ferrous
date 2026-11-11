// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// Playback controls, keyboard and wheel input, and the fullscreen viewers.
class TransportInputSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void fullscreenVisualizationInhibitsDisplaySleepOnlyWhileActive();
    void playbackControllerReadsTheBridgeClock();
    void windowPresenceFollowsTheScreensRefreshRate();
    void mainWindowStopsPositionFramesWhileHidden();
    void spaceTogglesPlaybackWhenTheLibraryHasFocus();
    void spaceTypesIntoTextFields();
    void playbackShortcutsWorkInTheViewers();
    void viewerShortcutsOpenAndCloseTheViewers();
    void fullscreenViewerWindowsAreMadeOnFirstOpenAndKept();
    void visualizationModeSwitchTakesKeyboardFocus();
    void visualizationModeSwitchStaysClearOfTheCornerOverlays();
    void visualizationModeSwitchShowsOnlyWithThePointerOrFocus();
    void visualizationModeSwitchShowsBeforeAnyAudio();
    void visualizationModeSwitchLeavesTheCornerToTheView();
    void viewersShowTheTrackInATitleStripOverTheWholeScreen();
    void fullscreenStripHidesWhatTheViewDrawsUnderIt();
    void viewerModeSwitchStaysWhileThePointerIsOnTheViewersControls();
    void fullscreenViewersGiveTheViewTheWholeScreen();
    void viewerStripHoldsItsTextAtALargeFont_data();
    void viewerStripHoldsItsTextAtALargeFont();
    void albumArtDetailsKeepBelowTheViewerControls_data();
    void albumArtDetailsKeepBelowTheViewerControls();
    void fullscreenVisualizationKeepsItsControlsBelowTheStrip();
    void viewersInTheMainWindowAreNamedInItsTitle();
    void viewerZoomShowsAReadoutAndAResetButton();
    void mainPaneZoomShowsAReadoutBesideTheModeSwitch();
    void resetZoomResetsOnlyTheShownPlotsZoom();
    void resetZoomIsOffAtAShortTracksStretchedDefault();
    void viewerControlsComeBackForTheKeyboard();
    void viewerKeysWorkFromEveryControl();
    void viewerControlsStayWhileOneHasFocus();
    void fullscreenViewersCanKeepTheirTitleStripHidden();
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
    // The clock ticks no faster than the screen the view is on refreshes.
    controller->setProperty("frameRate", 144.0);
    QCOMPARE(clock.frameRate(), 144.0);
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

void TransportInputSmokeTest::windowPresenceFollowsTheScreensRefreshRate() {
    WindowPresence presence;
    QSignalSpy changed(&presence, &WindowPresence::screenRefreshRateChanged);
    QCOMPARE(presence.screenRefreshRate(), 0.0);
    auto window = std::make_unique<QWindow>();
    QVERIFY(window->screen() != nullptr);
    const double refreshRate = window->screen()->refreshRate();
    QVERIFY(refreshRate > 0.0);
    presence.setWindow(window.get());
    QCOMPARE(presence.screenRefreshRate(), refreshRate);
    QCOMPARE(changed.count(), 1);
    // No window, no screen.
    presence.setWindow(nullptr);
    QCOMPARE(presence.screenRefreshRate(), 0.0);
    presence.setWindow(window.get());
    QCOMPARE(presence.screenRefreshRate(), refreshRate);
    window.reset();
    QCOMPARE(presence.screenRefreshRate(), 0.0);
    QCOMPARE(changed.count(), 4);
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
    // Paced by the refresh rate of the window's screen.
    QVERIFY(window->screen() != nullptr);
    const double refreshRate = window->screen()->refreshRate();
    QCOMPARE(clock.frameRate(), refreshRate > 0.0 ? refreshRate : PlaybackClock::kDefaultFrameRate);

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
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void TransportInputSmokeTest::visualizationModeSwitchTakesKeyboardFocus() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(viewer != nullptr && playPause != nullptr && bridge != nullptr);
    // A track plays: before any audio, the switch always shows.
    setPlayingTrack(bridge);
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
    // A track plays, so its lanes have their labels.
    setPlayingTrack(bridge);
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
    // Rolling: the plot's right edge is now. A track plays: before any
    // audio, the switch always shows.
    setPlayingTrack(bridge);
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

// Before any audio the pane is its bare floor, with no message, and the
// mode switch shows in its corner without the pointer, so a mode can be
// picked first. There are no lanes: no labels, no Mute and Solo.
void TransportInputSmokeTest::visualizationModeSwitchShowsBeforeAnyAudio() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveform = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && spectrogram != nullptr && waveform != nullptr);
    QCOMPARE(bridge->property("currentTrackPath").toString(), QString());
    QVERIFY(bridge->setProperty("showLevelMeter", false));
    // Per channel, where a track has a lane for each.
    QVERIFY(bridge->setProperty("spectrogramViewMode", 1));
    QTRY_VERIFY(spectrogram->isVisible());
    QQuickItem *modeSwitch =
        firstVisibleItemNamed(window->contentItem(), QStringLiteral("visualizationModeSwitch"));
    QQuickItem *waveformButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QQuickItem *spectrogramButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton0"));
    QVERIFY(modeSwitch != nullptr && waveformButton != nullptr && spectrogramButton != nullptr);

    // The pointer off the pane: the switch shows, in its corner, 4 px in.
    QTest::mouseMove(window, QPoint(4, window->height() - 4));
    QVERIFY(!sceneRectOf(spectrogram).contains(QPointF(4, window->height() - 4)));
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY(modeSwitch->isVisible());
    const QRectF pane = sceneRectOf(spectrogram);
    const QRectF plate = sceneRectOf(modeSwitch);
    QCOMPARE(plate.top() - pane.top(), 4.0);
    QCOMPARE(pane.right() - plate.right(), 4.0);
    // No message on the plot, and no lanes to label.
    QVERIFY(firstVisibleItemNamed(window->contentItem(), QStringLiteral("plotAnalysingText")) == nullptr);
    QCOMPARE(visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneHead")).size(), 0);

    // It works: a click picks the waveform, which still has no lanes.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRectOf(waveformButton).center().toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QTRY_VERIFY(waveform->isVisible());
    // The spectrogram goes once the waveform has faded in over it.
    QTRY_VERIFY(!spectrogram->isVisible());
    QVERIFY(waveformButton->property("checked").toBool());
    QTest::mouseMove(window, QPoint(4, window->height() - 4));
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QCOMPARE(visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneHead")).size(), 0);
    // And back.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRectOf(spectrogramButton).center().toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QTRY_VERIFY(spectrogram->isVisible());
    QTest::mouseMove(window, QPoint(4, window->height() - 4));

    // Once a track plays, the switch steps back to the pointer, and the
    // waveform's lanes have their labels.
    setPlayingTrack(bridge);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveform->isVisible());
    QTRY_VERIFY(!visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneHead")).isEmpty());
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
         QStringLiteral("Morning Tide Field — Ferrous Album Art"), "mediaPlot"},
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

        // A 32 px band on the plot's strip colour, opaque, with its edge
        // under it, over the top of the view.
        QObject *plotColors = qvariant_cast<QObject *>(palette->property(shown.plotColors));
        QVERIFY(plotColors != nullptr);
        QQuickItem *strip = visibleItemNamed(fullscreen, shown.stripName);
        QVERIFY2(strip != nullptr, name.constData());
        QTRY_COMPARE(sceneRectOf(strip), QRectF(0, 0, fullscreen->width(), 33));
        QCOMPARE(colorOf(strip, "color"), colorOf(plotColors, "plotStrip"));
        QCOMPARE(qAlpha(colorOf(strip, "color")), 255);
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

void TransportInputSmokeTest::viewerStripHoldsItsTextAtALargeFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("default font") << 0.0;
    QTest::newRow("24 pt") << 24.0;
}

// The strip is as tall as its line of text and 4 px above and below it,
// 32 px and its edge at the least: at 24 pt its text ran 6 px over the top
// of the screen. It still lies over the view, which keeps the whole
// window, and what keeps below it moves down with it.
void TransportInputSmokeTest::viewerStripHoldsItsTextAtALargeFont() {
    QFETCH(qreal, pointSize);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont larger = applicationFont;
        larger.setPointSizeF(pointSize);
        QGuiApplication::setFont(larger);
    }
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
    auto *albumArt = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && plotLayer != nullptr && albumArt != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", 1);

    struct Viewer {
        const char *open;
        const char *close;
        const char *openProperty;
        QString windowName;
        QString stripName;
        QString closeName;
        QString surfaceName;
    };
    const QList<Viewer> viewers{
        {"openSpectrogramViewer", "closeSpectrogramViewer", "spectrogramViewerOpen",
         QStringLiteral("spectrogramFullscreenWindow"), QStringLiteral("spectrogramFullscreenStrip"),
         QStringLiteral("spectrogramFullscreenCloseButton"), QStringLiteral("mainSpectrogramSurface")},
        {"openAlbumArtViewerForCurrentArt", "closeAlbumArtViewer", "albumArtViewerOpen",
         QStringLiteral("albumArtFullscreenWindow"), QStringLiteral("albumArtFullscreenStrip"),
         QStringLiteral("albumArtFullscreenCloseButton"), QStringLiteral("albumArtSurface")},
    };
    for (const Viewer &shown : viewers) {
        const QByteArray name = shown.windowName.toUtf8() + QByteArray(" at ") + QByteArray::number(pointSize);
        QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
        QTRY_VERIFY(viewer->property(shown.openProperty).toBool());
        QQuickWindow *fullscreen = viewerWindowNamed(engine, shown.windowName);
        QVERIFY(fullscreen != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(fullscreen));
        QQuickItem *strip = nullptr;
        QTRY_VERIFY((strip = visibleItemNamed(fullscreen, shown.stripName)) != nullptr);
        QQuickItem *close = visibleItemNamed(fullscreen, shown.closeName);
        QVERIFY2(close != nullptr, name.constData());

        qreal line = 0;
        QList<QQuickItem *> labels;
        for (const QString &labelName : {QStringLiteral("viewerStripTitle"), QStringLiteral("viewerStripContext"),
                                         QStringLiteral("viewerStripTime")}) {
            QQuickItem *label = itemNamed(strip, labelName);
            QVERIFY2(label != nullptr && label->isVisible(), name.constData());
            if (pointSize > 0) {
                QCOMPARE(qvariant_cast<QFont>(label->property("font")).pointSizeF(), pointSize);
            }
            line = std::max(line, label->implicitHeight());
            labels << label;
        }
        const qreal band = std::max({32.0, std::ceil(line) + 8.0, close->height() + 8.0});
        QTRY_COMPARE(sceneRectOf(strip), QRectF(0, 0, fullscreen->width(), band + 1.0));
        if (pointSize == 0) {
            QCOMPARE(band, 32.0);
        } else {
            QVERIFY2(band > 32.0, name.constData());
        }
        // Each text whole inside the band, over the edge under it.
        for (QQuickItem *label : std::as_const(labels)) {
            const QRectF rect = sceneRectOf(label);
            QVERIFY2(rect.top() >= 0.0 && rect.bottom() <= band,
                     qPrintable(QStringLiteral("%1: %2 from %3 to %4 in a %5 px band")
                                    .arg(QString::fromUtf8(name), label->objectName())
                                    .arg(rect.top()).arg(rect.bottom()).arg(band)));
        }
        // Close, centred in the band, 4 px in from the right.
        QCOMPARE(sceneRectOf(close).right(), fullscreen->width() - 4.0);
        QVERIFY(std::abs(sceneRectOf(close).center().y() - band / 2) <= 0.5);
        // Over the view, which keeps the whole window.
        auto *surface = window->findChild<QQuickItem *>(shown.surfaceName);
        if (surface == nullptr) {
            surface = fullscreen->findChild<QQuickItem *>(shown.surfaceName);
        }
        QVERIFY2(surface != nullptr, name.constData());
        QTRY_COMPARE(sceneRectOf(surface), QRectF(0, 0, fullscreen->width(), fullscreen->height()));
        // The controls under the strip keep below it.
        if (shown.surfaceName == QStringLiteral("albumArtSurface")) {
            QTRY_COMPARE(albumArt->property("infoTopInset").toReal(), band + 1.0);
        } else {
            QTRY_COMPARE(plotLayer->property("topInset").toReal(), band + 1.0);
        }

        QVERIFY(QMetaObject::invokeMethod(viewer, shown.close));
        QTRY_VERIFY(!viewer->property(shown.openProperty).toBool());
        QTRY_VERIFY(!fullscreen->isVisible());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

void TransportInputSmokeTest::albumArtDetailsKeepBelowTheViewerControls_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("default font") << 0.0;
    QTest::newRow("24 pt") << 24.0;
}

// The album art's details start below the viewer's controls, which grow
// with the font: in the main window, under the plate in its corner, 12 px
// below it at the default font as before; over the whole screen, under the
// title strip. At 24 pt the file's name lay 8 px under the plate's buttons.
void TransportInputSmokeTest::albumArtDetailsKeepBelowTheViewerControls() {
    QFETCH(qreal, pointSize);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont larger = applicationFont;
        larger.setPointSizeF(pointSize);
        QGuiApplication::setFont(larger);
    }
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
    auto *albumArt = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && albumArt != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());

    // In the main window.
    bridge->setProperty("viewerFullscreenMode", 0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(viewer->setProperty("albumArtInfoVisible", true));
    QQuickItem *controls = nullptr;
    QQuickItem *title = nullptr;
    QTRY_VERIFY((controls = visibleItemNamed(window, QStringLiteral("albumArtPopupControls"))) != nullptr);
    QTRY_VERIFY((title = visibleItemNamed(window, QStringLiteral("albumArtInfoTitle"))) != nullptr);
    const qreal plateBottom = sceneRectOf(controls).bottom();
    if (pointSize == 0) {
        QCOMPARE(plateBottom, 32.0);
        QTRY_COMPARE(sceneRectOf(title).top(), 44.0);
    } else {
        QVERIFY2(plateBottom > 32.0, qPrintable(QString::number(plateBottom)));
    }
    QTRY_COMPARE(albumArt->property("infoTopInset").toReal(), plateBottom);
    QTRY_VERIFY2(sceneRectOf(title).top() >= plateBottom + 12 - 0.5,
                 qPrintable(QStringLiteral("title at %1, plate to %2")
                                .arg(sceneRectOf(title).top()).arg(plateBottom)));
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());

    // Over the whole screen, with the title strip shown.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(bridge->setProperty("showFullscreenTitleStrip", true));
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(viewer->setProperty("albumArtInfoVisible", true));
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QTest::mouseMove(fullscreen, QPoint(300, 300));
    QQuickItem *strip = nullptr;
    QTRY_VERIFY((strip = visibleItemNamed(fullscreen, QStringLiteral("albumArtFullscreenStrip"))) != nullptr);
    QTRY_VERIFY((title = visibleItemNamed(fullscreen, QStringLiteral("albumArtInfoTitle"))) != nullptr);
    QTRY_VERIFY2(sceneRectOf(title).top() >= sceneRectOf(strip).bottom() + 12 - 0.5,
                 qPrintable(QStringLiteral("title at %1, strip to %2")
                                .arg(sceneRectOf(title).top()).arg(sceneRectOf(strip).bottom())));
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY(!fullscreen->isVisible());
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
    // The plot keeps its corner clear of the mode switch where it now is,
    // and of the 1 px edge around its plate.
    QTRY_COMPARE(spectrogram->reservedCorner().height(), 33.0 + 4.0 + 28.0 + 1.0);
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

// The title strip over the whole screen is opaque (#373 H5: the playhead
// and the spectrogram showed through it): a line the view draws the whole
// height, as the playhead and the crosshair are, ends at the strip's lower
// edge. The strip still draws over the view, which keeps its size.
void TransportInputSmokeTest::fullscreenStripHidesWhatTheViewDrawsUnderIt() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(bridge != nullptr && viewer != nullptr && palette != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("viewerFullscreenMode", 1);
    for (const int mode : {0, 1}) {
        const QByteArray where = mode == 0 ? "spectrogram" : "waveform";
        viewer->setProperty("visualizationMode", mode);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
        QVERIFY(fullscreen != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(fullscreen));
        QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
        auto *surface = window->findChild<QQuickItem *>(QStringLiteral("visualizationSurface"));
        QVERIFY(strip != nullptr && surface != nullptr);
        QTRY_COMPARE(surface->window(), fullscreen);
        QQuickItem *host = surface->parentItem();
        QTRY_COMPARE(strip->opacity(), 1.0);
        // The view under the strip, the whole window.
        QTRY_COMPARE(sceneRectOf(host), QRectF(0, 0, fullscreen->width(), fullscreen->height()));
        QObject *plotColors = qvariant_cast<QObject *>(palette->property(mode == 0 ? "spectrogramPlot"
                                                                                      : "waveformPlot"));
        const QColor stripColor = plotColors->property("plotStrip").value<QColor>();
        QCOMPARE(stripColor.alphaF(), 1.0f);

        // A line in the view, as the playhead, from its top to its bottom,
        // in the middle, clear of the strip's text and buttons.
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nRectangle { width: 1; color: \"#ff00ff\" }", QUrl());
        std::unique_ptr<QQuickItem> line(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(line != nullptr, qPrintable(component.errorString()));
        line->setParentItem(host);
        line->setZ(100);
        line->setX(std::round(host->width() / 2));
        line->setHeight(host->height());
        QTRY_VERIFY(fullscreen->isExposed());
        const QImage image = fullscreen->grabWindow();
        QVERIFY(!image.isNull());
        const double scale = double(image.width()) / fullscreen->width();
        const auto pixel = [&](double x, double y) {
            return image.pixelColor(int((x + 0.5) * scale), int((y + 0.5) * scale));
        };
        const double x = line->x();
        // Under the strip: its colour, as beside the line.
        for (int y = 1; y < 32; y += 3) {
            const QColor at = pixel(x, y);
            QVERIFY2(std::abs(at.red() - stripColor.red()) <= 1 && std::abs(at.green() - stripColor.green()) <= 1
                         && std::abs(at.blue() - stripColor.blue()) <= 1,
                     qPrintable(QStringLiteral("%1: %2 at y %3, the strip %4")
                                    .arg(QLatin1String(where), at.name(), QString::number(y), stripColor.name())));
        }
        // Below it, the line shows.
        QCOMPARE(pixel(x, 33 + 20).name(), QStringLiteral("#ff00ff"));
        line.reset();
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

// The pointer on the viewer's own controls is on the view (#373 H5: in the
// viewer in the window, the mode switch went as the pointer reached the
// corner plate beside it): the switch stays, in the window on the corner
// plate and its Close, and over the whole screen on the title strip. It
// shows with the viewer's controls, which it hides with.
void TransportInputSmokeTest::viewerModeSwitchStaysWhileThePointerIsOnTheViewersControls() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(bridge != nullptr && viewer != nullptr && layer != nullptr);
    QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(modeSwitch != nullptr);
    setPlayingTrack(bridge);
    for (const int fullscreenMode : {0, 1}) {
        const QByteArray where = fullscreenMode == 1 ? "over the whole screen" : "in the window";
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
        QQuickWindow *host = window;
        QQuickItem *controls = nullptr;
        QList<QQuickItem *> spots;
        if (fullscreenMode == 1) {
            host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
            QVERIFY(host != nullptr);
            QVERIFY(QTest::qWaitForWindowActive(host));
            controls = visibleItemNamed(host, QStringLiteral("spectrogramFullscreenStrip"));
            QVERIFY(controls != nullptr);
            spots = {controls, itemNamed(controls, QStringLiteral("spectrogramFullscreenCloseButton"))};
        } else {
            QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
            QTRY_VERIFY(popup->property("opened").toBool());
            controls = visibleItemNamed(window, QStringLiteral("spectrogramPopupControls"));
            QVERIFY(controls != nullptr);
            spots = {itemNamed(controls, QStringLiteral("spectrogramPopupCloseButton"))};
        }
        QTRY_COMPARE(layer->window(), host);
        // The pointer on the view reveals the switch.
        QTest::mouseMove(host, QPoint(host->width() / 2, host->height() / 2));
        QTRY_COMPARE(modeSwitch->property("revealLevel").toDouble(), 1.0);
        for (QQuickItem *spot : spots) {
            QVERIFY2(spot != nullptr, where.constData());
            const QPoint at = fullscreenMode == 1 && spot == controls
                ? QPoint(host->width() / 2, int(controls->height() / 2))
                : centerInWindow(spot);
            QTest::mouseMove(host, at);
            QTRY_VERIFY2(controls->property("hovered").toBool(), where.constData());
            // Longer than the switch's fade: it stays revealed.
            QTest::qWait(300);
            QVERIFY2(modeSwitch->property("revealed").toBool(), where.constData());
            QCOMPARE(modeSwitch->property("revealLevel").toDouble(), 1.0);
            QVERIFY(modeSwitch->isVisible());
        }
        // Off the view and its controls, it stays while the viewer's
        // controls show, as their part (#373 H6), and goes with them, at
        // their level.
        QTest::mouseMove(host, QPoint(-20, -20));
        QTest::qWait(300);
        QVERIFY2(modeSwitch->property("revealed").toBool(), where.constData());
        QCOMPARE(modeSwitch->property("revealLevel").toDouble(), 1.0);
        QObject *autoHide = window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
        QVERIFY(autoHide != nullptr);
        autoHide->setProperty("hideDelay", 1);
        QTRY_VERIFY2(!modeSwitch->isVisible(), where.constData());
        QCOMPARE(modeSwitch->opacity(), 0.0);
        autoHide->setProperty("hideDelay", 5000);
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

void TransportInputSmokeTest::viewerZoomShowsAReadoutAndAResetButton() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *resetAction = window->findChild<QObject *>(QStringLiteral("resetZoomAction"));
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

    // View › Reset Zoom brings the spectrogram back to its default zoom, as
    // it does the waveform.
    spectrogramSurface->setProperty("_widgetZoomLevel", 3.0);
    QCOMPARE(spectrogramZoom(), 3.0);
    QVERIFY(QMetaObject::invokeMethod(resetAction, "trigger"));
    QTRY_COMPARE(spectrogramZoom(), 1.0);

    // One plate order everywhere (#373 H5): [Zoom ↺] [mode] [✕], in both
    // viewers as in the main pane. The zoom is the plot's own plate, left of
    // the mode switch; the viewer's controls have Close and no zoom.
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(layer != nullptr);
    QQuickItem *readout = itemNamed(layer, QStringLiteral("plotZoomReadout"));
    QQuickItem *reset = itemNamed(layer, QStringLiteral("plotZoomResetButton"));
    QQuickItem *zoomPlate = itemNamed(layer, QStringLiteral("plotZoomPlate"));
    QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(readout != nullptr && reset != nullptr && zoomPlate != nullptr && modeSwitch != nullptr);
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
        QCOMPARE(layer->window(), host);
        QQuickItem *close = itemNamed(controls, fullscreenMode == 1
                                                    ? QStringLiteral("spectrogramFullscreenCloseButton")
                                                    : QStringLiteral("spectrogramPopupCloseButton"));
        QVERIFY(close != nullptr);
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
            QVERIFY(readout->isVisible() && reset->isVisible());
            QCOMPARE(readout->property("text").toString(),
                     mode == 0 ? QStringLiteral("Zoom 200%") : QStringLiteral("Zoom 400%"));
            // None in the viewer's own controls.
            const QList<QQuickItem *> ownReadouts = itemsNamed(controls, QStringLiteral("viewerZoomReadout"));
            for (QQuickItem *own : ownReadouts) {
                QVERIFY(!own->isVisible());
            }
            // Laid out: the readout, Reset, the mode switch 4 px right of
            // them, and Close right of that on the plate in the corner, or
            // over the whole screen on the strip above them.
            QTRY_VERIFY(sceneRectOf(readout).right() <= sceneRectOf(reset).left()
                        && sceneRectOf(zoomPlate).right() + 4 == sceneRectOf(modeSwitch).left());
            if (fullscreenMode == 1) {
                QVERIFY(sceneRectOf(close).bottom() <= sceneRectOf(modeSwitch).top());
                QVERIFY(sceneRectOf(close).right() >= sceneRectOf(modeSwitch).right());
            } else {
                QVERIFY2(sceneRectOf(modeSwitch).right() < sceneRectOf(close).left(),
                         qPrintable(QStringLiteral("switch ends at %1, Close starts at %2")
                                        .arg(sceneRectOf(modeSwitch).right()).arg(sceneRectOf(close).left())));
                QCOMPARE(sceneRectOf(controls).top(), sceneRectOf(modeSwitch).top());
            }
            QVERIFY(sceneRectOf(zoomPlate).left() >= 0 && sceneRectOf(close).right() <= host->width());
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

// The main pane shows its zoom while it is zoomed (#373 H4: nothing did,
// with the scale off): the viewers' readout and Reset, on a plate 4 px left
// of the mode switch, there also while the switch hides. The plot keeps its
// place and size; its overlays keep clear of the plate. A viewer shows the
// same plate (#373 H5: one plate order everywhere).
void TransportInputSmokeTest::mainPaneZoomShowsAReadoutBesideTheModeSwitch() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("visualizationSurface"));
    QVERIFY(bridge && viewer && spectrogramSurface && waveformSurface && layer && surface);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    QQuickItem *plate = itemNamed(layer, QStringLiteral("plotZoomPlate"));
    QQuickItem *readout = itemNamed(layer, QStringLiteral("plotZoomReadout"));
    QQuickItem *reset = itemNamed(layer, QStringLiteral("plotZoomResetButton"));
    QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(plate && readout && reset && modeSwitch);
    // The mode switch hidden, as with the pointer away from the pane; the
    // pointer comes back below.
    modeSwitch->setProperty("revealed", false);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);

    // Nothing at the default zoom.
    QVERIFY(!plate->isVisible());
    const QRectF plotBefore = sceneRectOf(spectrogramSurface);
    const QSizeF cornerBefore = surface->property("reservedCorner").toSizeF();

    for (const int mode : {0, 1}) {
        viewer->setProperty("visualizationMode", mode);
        QTRY_VERIFY(!surface->property("modeSwitching").toBool());
        QVERIFY(!plate->isVisible());
        if (mode == 0) {
            spectrogramSurface->setProperty("_widgetZoomLevel", 2.44);
        } else {
            waveform->setZoomLevel(4.0);
        }
        // At once, with the viewers' wording, in the plot's colours.
        QVERIFY(plate->isVisible());
        QCOMPARE(plate->opacity(), 1.0);
        QVERIFY(readout->isVisible() && reset->isVisible());
        QCOMPARE(readout->property("text").toString(),
                 mode == 0 ? QStringLiteral("Zoom 244%") : QStringLiteral("Zoom 400%"));
        QCOMPARE(reset->property("text").toString(), QStringLiteral("Reset Zoom"));
        QCOMPARE(plate->property("plotColors").value<QObject *>(),
                 layer->property("plotColors").value<QObject *>());
        // 4 px left of the switch, on its top, also while the switch hides
        // (the click on Reset below puts the pointer on the pane, which
        // reveals it).
        if (mode == 0) {
            QCOMPARE(modeSwitch->opacity(), 0.0);
        }
        QTRY_COMPARE(sceneRectOf(plate).right() + 4, sceneRectOf(modeSwitch).left());
        QCOMPARE(sceneRectOf(plate).top(), sceneRectOf(modeSwitch).top());
        QCOMPARE(sceneRectOf(plate).height(), sceneRectOf(modeSwitch).height());
        QVERIFY(sceneRectOf(readout).right() <= sceneRectOf(reset).left());
        // The plot stays where it is; its overlays keep clear of the plate.
        QCOMPARE(sceneRectOf(spectrogramSurface), plotBefore);
        const QSizeF corner = surface->property("reservedCorner").toSizeF();
        QCOMPARE(corner.width(), cornerBefore.width() + 4 + plate->width());
        QCOMPARE(corner.height(), cornerBefore.height());

        // Reset: the default zoom, and the plate goes.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          reset->mapToScene(QPointF(reset->width() / 2, reset->height() / 2)).toPoint());
        if (mode == 0) {
            QTRY_COMPARE(spectrogramSurface->property("zoomLevel").toDouble(), 1.0);
        } else {
            QTRY_COMPARE(waveform->zoomLevel(), 1.0);
        }
        QTRY_VERIFY(!plate->isVisible());
        QCOMPARE(surface->property("reservedCorner").toSizeF(), cornerBefore);
    }

    // In a viewer, the same plate shows, left of the mode switch, and none
    // in the viewer's controls.
    viewer->setProperty("visualizationMode", 0);
    bridge->setProperty("viewerFullscreenMode", 0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
    spectrogramSurface->setProperty("_viewerZoomLevel", 2.0);
    QQuickItem *controls = visibleItemNamed(window, QStringLiteral("spectrogramPopupControls"));
    QVERIFY(controls != nullptr);
    QTRY_VERIFY(plate->isVisible());
    QVERIFY(itemsNamed(controls, QStringLiteral("viewerZoomReadout")).isEmpty());
    QTRY_COMPARE(sceneRectOf(plate).right() + 4, sceneRectOf(modeSwitch).left());

    // One row of plates in the corner (#373 H6): the zoom's, the mode
    // switch's and the viewer's Close on the one plot plate, in its fill and
    // edge, as tall and on one top, 4 px apart.
    QQuickItem *cornerPlate = itemNamed(controls, QStringLiteral("viewerCornerPlate"));
    QVERIFY(cornerPlate != nullptr);
    QTRY_COMPARE(sceneRectOf(modeSwitch).right() + 4, sceneRectOf(cornerPlate).left());
    for (QQuickItem *other : {modeSwitch, cornerPlate}) {
        QCOMPARE(sceneRectOf(other).top(), sceneRectOf(plate).top());
        QCOMPARE(sceneRectOf(other).height(), sceneRectOf(plate).height());
        for (const char *part : {"plotPlateFill", "plotPlateEdge"}) {
            QQuickItem *mine = itemNamed(plate, QString::fromLatin1(part));
            QQuickItem *theirs = itemNamed(other, QString::fromLatin1(part));
            QVERIFY(mine != nullptr && theirs != nullptr);
            QCOMPARE(theirs->property("color").value<QColor>(), mine->property("color").value<QColor>());
            QCOMPARE(theirs->property("radius").toDouble(), mine->property("radius").toDouble());
            QCOMPARE(theirs->property("border").value<QObject *>()->property("color").value<QColor>(),
                     mine->property("border").value<QObject *>()->property("color").value<QColor>());
        }
    }
    // The readout 6 px in from the plate, and 6 px from Reset's glyph box.
    QQuickItem *resetGlyph = itemNamed(reset, QStringLiteral("plotToolButtonIcon"));
    QVERIFY(resetGlyph != nullptr);
    const double textLeft = sceneRectOf(readout).left() + readout->property("leftPadding").toDouble();
    const double textRight = textLeft + readout->property("contentWidth").toDouble();
    QCOMPARE(textLeft - sceneRectOf(plate).left(), 6.0);
    QCOMPARE(sceneRectOf(resetGlyph).left() - textRight, 6.0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

void TransportInputSmokeTest::resetZoomResetsOnlyTheShownPlotsZoom() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *resetAction = window->findChild<QObject *>(QStringLiteral("resetZoomAction"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && resetAction != nullptr
            && spectrogramSurface != nullptr && waveformSurface != nullptr);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    QCOMPARE(resetAction->property("shortcut").value<QKeySequence>(),
             QKeySequence(QStringLiteral("Ctrl+0")));

    // The spectrogram with two channels: the old Reset Visualization View
    // cleared them and drew the spectrogram afresh.
    QVariant stereo;
    QVERIFY(QMetaObject::invokeMethod(spectrogramSurface, "descriptorsForChannelCount",
                                      Q_RETURN_ARG(QVariant, stereo), Q_ARG(QVariant, 2)));
    spectrogramSurface->setProperty("channelDescriptors", stereo);
    QCOMPARE(spectrogramSurface->property("laneCount").toInt(), 2);
    QSignalSpy triggered(resetAction, SIGNAL(triggered(QObject *)));

    // `zoom` sets the shown plot's zoom in the main window or a viewer;
    // `zoomOf` reads it.
    const auto zoom = [&](int mode, double level) {
        if (mode == 0) {
            spectrogramSurface->setProperty(
                viewer->property("spectrogramViewerOpen").toBool() ? "_viewerZoomLevel" : "_widgetZoomLevel",
                level);
        } else {
            waveform->setZoomLevel(level);
        }
    };
    const auto zoomOf = [&](int mode) {
        return mode == 0 ? spectrogramSurface->property("zoomLevel").toDouble() : waveform->zoomLevel();
    };
    // In `host`: disabled at the default zoom, enabled when zoomed, and
    // Ctrl+0 resets the shown plot's zoom and nothing else.
    const auto check = [&](QQuickWindow *host, const char *where) {
        for (const int mode : {0, 1}) {
            const QByteArray label = QByteArray(where) + (mode == 0 ? " spectrogram" : " waveform");
            viewer->setProperty("visualizationMode", mode);
            QTRY_COMPARE(zoomOf(mode), 1.0);
            QVERIFY2(!resetAction->property("enabled").toBool(), label.constData());
            // At 1×, the key does nothing.
            const int before = triggered.count();
            QTest::keyClick(host, Qt::Key_0, Qt::ControlModifier);
            QCoreApplication::processEvents();
            QVERIFY2(triggered.count() == before, label.constData());

            zoom(mode, 3.0);
            QCOMPARE(zoomOf(mode), 3.0);
            QVERIFY2(resetAction->property("enabled").toBool(), label.constData());
            QTest::keyClick(host, Qt::Key_0, Qt::ControlModifier);
            QTRY_COMPARE(triggered.count(), before + 1);
            QCOMPARE(zoomOf(mode), 1.0);
            QVERIFY2(!resetAction->property("enabled").toBool(), label.constData());
            // The spectrogram keeps its channels: nothing was cleared.
            QCOMPARE(spectrogramSurface->property("laneCount").toInt(), 2);
            QCOMPARE(spectrogramSurface->property("channelDescriptors").toList().size(), 2);
        }
        viewer->setProperty("visualizationMode", 0);
    };

    // The main window.
    window->contentItem()->forceActiveFocus();
    check(window, "main window");

    // The viewer inside the main window.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    check(window, "viewer in the window");
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());

    // The viewer over the whole screen.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(spectrogramSurface->window() != nullptr && spectrogramSurface->window() != window);
    QQuickWindow *fullscreen = spectrogramSurface->window();
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    check(fullscreen, "viewer over the screen");

    // The viewer's Reset, beside the mode switch, does the same zoom-only
    // reset.
    QQuickItem *layer = visibleItemNamed(fullscreen, QStringLiteral("plotLayer"));
    QVERIFY(layer != nullptr);
    QQuickItem *reset = itemNamed(layer, QStringLiteral("plotZoomResetButton"));
    QVERIFY(reset != nullptr);
    zoom(0, 2.0);
    QTRY_VERIFY(reset->isVisible());
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(reset));
    QTRY_COMPARE(zoomOf(0), 1.0);
    QCOMPARE(spectrogramSurface->property("laneCount").toInt(), 2);
    QTest::keyClick(fullscreen, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

void TransportInputSmokeTest::resetZoomIsOffAtAShortTracksStretchedDefault() {
    // A short track stretched to the plot's width is at its default zoom:
    // Reset Zoom is off, and the zoom is reckoned from it.
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *resetAction = window->findChild<QObject *>(QStringLiteral("resetZoomAction"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && resetAction != nullptr
            && spectrogramSurface != nullptr);
    setPlayingTrack(bridge);
    viewer->setProperty("visualizationMode", 0);
    QVERIFY(bridge->property("stretchShortTracks").toBool());
    SpectrogramItem *item = nullptr;
    QTRY_VERIFY((item = qobject_cast<SpectrogramItem *>(findQuickItemByObjectName(
                     spectrogramSurface, QStringLiteral("spectrogramPaneItem")))) != nullptr);
    QTRY_VERIFY(item->width() > 100.0);
    const auto width = static_cast<std::uint32_t>(std::floor(item->width()));

    // A second at 48 kHz.
    constexpr int bins = 4;
    constexpr quint64 frames = 48'000;
    const int end = static_cast<int>(ferrous_ffi_spectrogram_decoded_end_column(frames, 8192, 1.0f, 48'000));
    item->feedPrecomputedChunk(QByteArray(), bins, 0, 0, 0, end, 48'000, 1024, false, true, 5, true,
                               0, 0, 8192, frames, end);
    item->feedPrecomputedChunk(QByteArray(16 * bins, '\x40'), bins, 0, 16, 0, end, 48'000, 1024,
                               false, false, 5, false, 0, 0, 8192, frames, end);
    const double fit = ferrous_ffi_spectrogram_fit_zoom(frames, 8192, 48'000, width);
    QVERIFY(fit > 1.0);
    QTRY_COMPARE(spectrogramSurface->property("zoomLevel").toDouble(), fit);
    QObject *visualization = window->findChild<QObject *>(QStringLiteral("visualizationSurface"));
    QVERIFY(visualization != nullptr);
    QCOMPARE(visualization->property("zoomLevel").toDouble(), 1.0);
    QVERIFY(!resetAction->property("enabled").toBool());

    // Zoomed out from it to today's scale, and back.
    QVERIFY(spectrogramSurface->setProperty("_widgetZoomLevel", 1.0));
    QVERIFY(resetAction->property("enabled").toBool());
    QVERIFY(qAbs(visualization->property("zoomLevel").toDouble() - 1.0 / fit) < 1e-9);
    QVERIFY(QMetaObject::invokeMethod(resetAction, "trigger"));
    QTRY_COMPARE(spectrogramSurface->property("zoomLevel").toDouble(), fit);
    QCOMPARE(item->zoomLevel(), fit);
    QVERIFY(!resetAction->property("enabled").toBool());
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
    QQuickItem *pane = itemNamed(window->contentItem(), QStringLiteral("spectrogramPane"));
    QQuickItem *surface = itemNamed(window->contentItem(), QStringLiteral("visualizationSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && pane != nullptr && surface != nullptr);
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
                // The viewer in the window takes the visualization once the
                // pane has kept its last frame, a frame after it opens.
                if (shown == Shown::Visualization
                    && !QTest::qWaitFor([&]() { return !pane->isAncestorOf(surface); })) {
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

std::unique_ptr<QmlSmokeTest> makeTransportInputSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<TransportInputSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_transport_input.moc"
