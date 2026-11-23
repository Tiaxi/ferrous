// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"
#include "TestMainWindow.h"
#include "TestColor.h"

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
    void changedShortcutsApplyToTheWindowAndTheViewers();
    void viewerShortcutsOpenAndCloseTheViewers();
    void fullscreenViewerWindowsAreMadeOnFirstOpenAndKept();
    void visualizationModeSwitchTakesKeyboardFocus();
    void visualizationModeSwitchStaysClearOfTheCornerOverlays();
    void visualizationModeSwitchShowsOnlyWithThePointerOrFocus();
    void visualizationModeSwitchShowsBeforeAnyAudio();
    void paneAtRestIsAStereoInstrument();
    void paneAtRestKeepsItsLinesClearOfThePlates_data();
    void paneAtRestKeepsItsLinesClearOfThePlates();
    void visualizationModeSwitchLeavesTheCornerToTheView();
    void crosshairGivesWayToThePlotsPlates();
    void viewersShowTheTrackInATitleStripOverTheWholeScreen();
    void fullscreenStripHidesWhatTheViewDrawsUnderIt();
    void viewerModeSwitchStaysWhileThePointerIsOnTheViewersControls();
    void fullscreenViewersGiveTheViewTheWholeScreen();
    void viewerStripHoldsItsTextAtALargeFont();
    void albumArtDetailsKeepBelowTheViewerControls();
    void fullscreenVisualizationKeepsItsControlsBelowTheStrip();
    void viewersInTheMainWindowAreNamedInItsTitle();
    void viewerZoomShowsAReadoutAndAResetButton();
    void mainPaneZoomShowsAReadoutBesideTheModeSwitch();
    void mainPaneCornerShowsAndHidesAsOneRow();
    void mainPaneCornerAndCrosshairShareOneHover_data();
    void mainPaneCornerAndCrosshairShareOneHover();
    void laneSeamIsOnThePlot_data();
    void laneSeamIsOnThePlot();
    void resetZoomResetsOnlyTheShownPlotsZoom();
    void resetZoomIsOffAtAShortTracksStretchedDefault();
    void movedViewSaysSoInTheCorner_data();
    void movedViewSaysSoInTheCorner();
    void notFollowingLeavesOneWay_data();
    void notFollowingLeavesOneWay();
    void notFollowingComesInWithThePlate();
    void notFollowingArrivesAsItLeaves_data();
    void notFollowingArrivesAsItLeaves();
    void notFollowingMovesAsOnePlate_data();
    void notFollowingMovesAsOnePlate();
    void cornerHoldsWhileAutoscrollRuns_data();
    void cornerHoldsWhileAutoscrollRuns();
    void laneHeadsStayQuietThroughAPan_data();
    void laneHeadsStayQuietThroughAPan();
    void zoomPlateTextIsDrawnAsThePlotPlatesIs();
    void crosshairGivesWayToAutoscroll_data();
    void crosshairGivesWayToAutoscroll();
    void plotsTellFollowingAgainInOneOrder_data();
    void plotsTellFollowingAgainInOneOrder();
    void seekBarBandLandsWithThePlot_data();
    void seekBarBandLandsWithThePlot();
    void zoomPlateSpacesItsDividerEvenly();
    void movedViewSaysSoInTheViewers_data();
    void movedViewSaysSoInTheViewers();
    void followPlayheadBringsTheShownPlotBack();
    void seekBarMarksTheZoomedViewOfTheShownPlot();
    void fTypesInTheLibraryAndFollowsThePlayheadElsewhere();
    void usingThePlotTakesTheFocusFromTheLibrary();
    void viewerKeysWorkAfterAPressOnItsPlot();
    void panShowsAClosedHandInThePaneAndTheViewers();
    void viewerControlsComeBackForTheKeyboard();
    void viewerKeysWorkFromEveryControl();
    void viewerControlsStayWhileOneHasFocus();
    void fullscreenViewersCanKeepTheirTitleStripHidden();
    void fullscreenVisualizationStripHoldsTheModeSwitch();
    void viewerButtonsRingOnlyForTheKeyboard_data();
    void viewerButtonsRingOnlyForTheKeyboard();
    void albumArtViewerClickedDetailsLetTheControlsHide();
    void viewerCornerPlateClearsTheLevelMeter_data();
    void viewerCornerPlateClearsTheLevelMeter();
    void viewerResetIsTheSameGlyphWindowedAndInTheStrip();
    void viewerToolbarIsOneVocabularyWindowedAndInTheStrip();
    void fullscreenLaneHeadsAreOnTheStripsTypeScale_data();
    void fullscreenLaneHeadsAreOnTheStripsTypeScale();
    void fullscreenPlotTextIsOnTheStripsTypeScale_data();
    void fullscreenPlotTextIsOnTheStripsTypeScale();
    void viewerStripButtonsFollowItsType_data();
    void viewerStripButtonsFollowItsType();
    void viewerStripIsOnOneTypeScale_data();
    void viewerDragsCountAsPointerActivity_data();
    void viewerDragsCountAsPointerActivity();
    void viewerStripIsOnOneTypeScale();
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
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
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

// The shortcuts the settings keep for an action replace its defaults at
// once: its own, which its menu item shows, the second one, which the
// window's AlternateShortcuts take, and both in a viewer, which repeats the
// window's (#458). An action cleared has none; one left out keeps its
// defaults, a standard key's following the platform's.
void TransportInputSmokeTest::changedShortcutsApplyToTheWindowAndTheViewers() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QObject *stop = window->findChild<QObject *>(QStringLiteral("stopAction"));
    QObject *open = window->findChild<QObject *>(QStringLiteral("openFilesAction"));
    QObject *find = window->findChild<QObject *>(QStringLiteral("findAction"));
    QVERIFY(bridge != nullptr && viewer != nullptr && playPause != nullptr && stop != nullptr
            && open != nullptr && find != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    QSignalSpy stopped(stop, SIGNAL(triggered(QObject*)));
    const auto bindingsOf = [](QObject *action) {
        return action->property("keyBindings").toStringList();
    };
    const auto shortcutOf = [](QObject *action) {
        return action->property("shortcut").value<QKeySequence>();
    };
    const auto portable = [](QKeySequence::StandardKey key) {
        QStringList texts;
        for (const QKeySequence &binding : QKeySequence::keyBindings(key)) {
            texts << binding.toString(QKeySequence::PortableText);
        }
        return texts;
    };

    // The defaults.
    QCOMPARE(bindingsOf(playPause), QStringList{QStringLiteral("Space")});
    QVERIFY(!playPause->property("hasCustomKeyBindings").toBool());
    QCOMPARE(bindingsOf(open), portable(QKeySequence::Open));
    QCOMPARE(shortcutOf(open), QKeySequence::keyBindings(QKeySequence::Open).value(0));
    QCOMPARE(bindingsOf(find), portable(QKeySequence::Find));

    bridge->setProperty(
        "keyboardShortcuts",
        QVariantMap{{QStringLiteral("play_pause"), QStringLiteral("Ctrl+Alt+P; Ctrl+Alt+J")},
                    {QStringLiteral("stop"), QString()},
                    {QStringLiteral("open_files"), QStringLiteral("Ctrl+Alt+O")},
                    // An action this build does not have.
                    {QStringLiteral("future_action"), QStringLiteral("Ctrl+Alt+K")}});
    QCOMPARE(bindingsOf(playPause),
             (QStringList{QStringLiteral("Ctrl+Alt+P"), QStringLiteral("Ctrl+Alt+J")}));
    QVERIFY(playPause->property("hasCustomKeyBindings").toBool());
    QCOMPARE(shortcutOf(playPause), QKeySequence(QStringLiteral("Ctrl+Alt+P")));
    QVERIFY(bindingsOf(stop).isEmpty());
    QVERIFY(shortcutOf(stop).isEmpty());
    QCOMPARE(bindingsOf(open), QStringList{QStringLiteral("Ctrl+Alt+O")});
    // Left out, so its standard key's.
    QCOMPARE(bindingsOf(find), portable(QKeySequence::Find));

    // In the main window. A shortcut is taken as its key is sent, so a key
    // that triggers nothing has triggered nothing once it is sent.
    window->contentItem()->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(toggled.count(), 1);
    QTest::keyClick(window, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(toggled.count(), 2);
    QTest::keyClick(window, Qt::Key_Space);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(toggled.count(), 2);
    QCOMPARE(stopped.count(), 0);

    // In the viewer, as a modal popup that blocks the window's shortcuts.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(toggled.count(), 3);
    QTest::keyClick(window, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(toggled.count(), 4);
    QTest::keyClick(window, Qt::Key_Space);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(toggled.count(), 4);
    QCOMPARE(stopped.count(), 0);

    // Back to the defaults, at once, also in the open viewer.
    bridge->setProperty("keyboardShortcuts", QVariantMap{});
    QCOMPARE(bindingsOf(playPause), QStringList{QStringLiteral("Space")});
    QCOMPARE(bindingsOf(open), portable(QKeySequence::Open));
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(toggled.count(), 5);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(stopped.count(), 1);
    QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QCOMPARE(toggled.count(), 5);
    QTest::keyClick(window, Qt::Key_Escape);
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
        // The pane's is the plot's; the viewer's, in its corner plate.
        QQuickItem *modeSwitchItem = firstVisibleItemNamed(
            controlsRoot, closeButtonName.isEmpty() ? QStringLiteral("visualizationModeSwitch")
                                                    : QStringLiteral("spectrogramPopupModeSwitch"));
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
                    // In the top right corner, off the centre line.
                    QVERIFY(view.contains(modeSwitch));
                    QVERIFY(modeSwitch.left() > view.center().x());
                    QVERIFY(modeSwitch.top() >= view.top());
                    QVERIFY(modeSwitch.top() - view.top() < margin);
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
    // Over the whole screen, the viewer's controls, the mode switch among
    // them, are in the title strip over the view's top edge, so the plot's
    // corner has none: the lanes' heads and the counter keep clear of
    // nothing there.
    QQuickItem *strip =
        firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QCOMPARE(sceneRectOf(spectrogramSurface).top(), 0.0);
    QCOMPARE(sceneRectOf(strip).top(), 0.0);
    QVERIFY(sceneRectOf(strip).contains(sceneRectOf(firstVisibleItemNamed(
        fullscreen->contentItem(), QStringLiteral("spectrogramFullscreenCloseButton")))));
    QQuickItem *stripModeSwitch =
        firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("spectrogramStripModeSwitch"));
    QVERIFY(stripModeSwitch != nullptr);
    QVERIFY(sceneRectOf(strip).contains(sceneRectOf(stripModeSwitch)));
    QCOMPARE(firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("visualizationModeSwitch")), nullptr);
    for (const int visualizationMode : {0, 1}) {
        viewer->setProperty("visualizationMode", visualizationMode);
        QQuickItem *surface = visualizationMode == 0 ? spectrogramSurface : waveformSurface;
        QTRY_VERIFY(surface->isVisible());
        QTRY_COMPARE(surface->property("reservedCorner").toSizeF(), QSizeF(0, 0));
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
    // The fades in the main window in animation time, each to its end.
    std::optional<SteppedAnimationDriver> animations(std::in_place);
    const auto settle = [&animations]() { (void)animations->step(1000); };

    // The pointer away: nothing on the plot's newest edge, but the switch is
    // still a Tab stop with its name.
    QTest::mouseMove(window, away);
    settle();
    QCOMPARE(modeSwitch->opacity(), 0.0);
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
    settle();
    QCOMPARE(modeSwitch->opacity(), 1.0);
    QTest::mouseMove(window, away);
    settle();
    QCOMPARE(modeSwitch->opacity(), 0.0);
    QTest::mouseMove(window, QPoint(static_cast<int>(pane.left()) + 60,
                                    static_cast<int>(pane.bottom()) - 10));
    settle();
    QCOMPARE(modeSwitch->opacity(), 1.0);
    // On the switch itself, it stays.
    QTest::mouseMove(window, sceneRectOf(spectrogramButton).center().toPoint());
    QTRY_VERIFY(modeSwitch->property("pointerHovered").toBool());
    settle();
    QCOMPARE(modeSwitch->opacity(), 1.0);
    QTest::mouseMove(window, away);
    settle();
    QCOMPARE(modeSwitch->opacity(), 0.0);

    // Tab focus shows it, and it stays while focus is on either button.
    QQuickItem *beforeSwitch = spectrogramButton->nextItemInFocusChain(false);
    QVERIFY(beforeSwitch != nullptr);
    beforeSwitch->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(spectrogramButton->hasActiveFocus());
    QVERIFY(modeSwitch->property("focusInside").toBool());
    settle();
    QCOMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY(!switchAccessible->state().invisible);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(waveformButton->hasActiveFocus());
    settle();
    QCOMPARE(modeSwitch->opacity(), 1.0);
    // Focus leaving it hides it again.
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(!waveformButton->hasActiveFocus());
    QVERIFY(!modeSwitch->property("focusInside").toBool());
    settle();
    QCOMPARE(modeSwitch->opacity(), 0.0);

    // Ctrl+1 and Ctrl+2 pick the modes while it is hidden.
    window->contentItem()->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QCOMPARE(modeSwitch->property("selectedMode").toInt(), 1);
    settle();
    QCOMPARE(modeSwitch->opacity(), 0.0);
    QTest::keyClick(window, Qt::Key_1, Qt::ControlModifier);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QCOMPARE(modeSwitch->property("selectedMode").toInt(), 0);
    settle();
    QCOMPARE(modeSwitch->opacity(), 0.0);

    // In the viewer in the main window, the pointer on the view shows it
    // too, with the viewer's controls, which come and go on real time.
    animations.reset();
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

// Before any audio the pane is at rest, with no message, and the mode
// switch shows in its corner without the pointer, so a mode can be picked
// first. Per channel, its lanes are a stereo track's, named L and R, with
// no Mute and Solo, as there is nothing to mute.
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

    // The pointer off the pane: the switch shows, in its corner, as far in
    // from the top as the lane heads' plates and as far from the right.
    QTest::mouseMove(window, QPoint(4, window->height() - 4));
    QVERIFY(!sceneRectOf(spectrogram).contains(QPointF(4, window->height() - 4)));
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY(modeSwitch->isVisible());
    const QRectF pane = sceneRectOf(spectrogram);
    const QRectF plate = sceneRectOf(modeSwitch);
    QQuickItem *laneHead = nullptr;
    QTRY_VERIFY((laneHead = firstVisibleItemNamed(window->contentItem(), QStringLiteral("plotLaneHead"))) != nullptr);
    QCOMPARE(plate.top(), sceneRectOf(laneHead).top());
    QCOMPARE(pane.right() - plate.right(), plate.top() - pane.top());
    // No message on the plot; a stereo track's lanes, named, without
    // Mute and Solo.
    QVERIFY(firstVisibleItemNamed(window->contentItem(), QStringLiteral("plotAnalysingText")) == nullptr);
    const auto restLanes = [&]() -> QString {
        const QList<QQuickItem *> heads = visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneHead"));
        QStringList labels;
        for (QQuickItem *head : heads) {
            labels.push_back(head->property("label").toString());
        }
        if (labels != QStringList{QStringLiteral("L"), QStringLiteral("R")}) {
            return QStringLiteral("lanes %1").arg(labels.join(QLatin1Char(',')));
        }
        if (!visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneButtons")).isEmpty()) {
            return QStringLiteral("Mute and Solo show");
        }
        return {};
    };
    QTRY_VERIFY2(restLanes().isEmpty(), qPrintable(restLanes()));

    // It works: a click picks the waveform, which still has no lanes.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRectOf(waveformButton).center().toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QTRY_VERIFY(waveform->isVisible());
    // The spectrogram stays live under the waveform once that has faded
    // in over it: at 0, taking no input.
    QTRY_COMPARE(spectrogram->opacity(), 0.0);
    QVERIFY(!spectrogram->isEnabled());
    QVERIFY(spectrogram->z() < waveform->z());
    QVERIFY(waveformButton->property("checked").toBool());
    QTest::mouseMove(window, QPoint(4, window->height() - 4));
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QTRY_VERIFY2(restLanes().isEmpty(), qPrintable(restLanes()));
    // And back.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRectOf(spectrogramButton).center().toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QTRY_VERIFY(spectrogram->isVisible());
    QTest::mouseMove(window, QPoint(4, window->height() - 4));

    // Once a track plays, the switch steps back to the pointer, and the
    // waveform's lanes have their labels, with Mute and Solo.
    setPlayingTrack(bridge);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveform->isVisible());
    QTRY_VERIFY(!visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneHead")).isEmpty());
    QTRY_VERIFY(!visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneButtons")).isEmpty());
}

void TransportInputSmokeTest::paneAtRestKeepsItsLinesClearOfThePlates_data() {
    // The application font's size, or 0 for the default.
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("default font") << 0.0;
    // 24 pt: the lanes shorter, two lines each.
    QTest::newRow("24 pt") << 24.0;
}

// The never-played pane reads as silent lanes: each has the same lines, at
// the same rows from its top, evenly spaced, each the lane's whole width,
// under the plates over the pane, which none is cut or dimmed for (each
// lane's first line was dimmer and cut short to a length of its own, beside
// whole ones, and read as broken): three, or fewer where the first of three
// would be beside a plate. The seam between the lanes at rest is a 3 px
// groove in the plot's deep violet, centred on the 1 px seam and drawn over
// the lanes, whose heights stay; playing, the seam is 1 px again. At the
// default font, mixed and per channel at 1600x1000 and 1000x720, and per
// channel at 24 pt, in turn in one window.
void TransportInputSmokeTest::paneAtRestKeepsItsLinesClearOfThePlates() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(bridge && spectrogram);
    QCOMPARE(bridge->property("currentTrackPath").toString(), QString());
    QVERIFY(bridge->setProperty("showSpectrogramScale", false));
    const auto panes = [&]() {
        QList<SpectrogramItem *> found;
        for (QQuickItem *item : itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"))) {
            found.push_back(qobject_cast<SpectrogramItem *>(item));
        }
        return found;
    };
    const auto grooves = [&]() {
        return visibleItemsNamed(spectrogram, QStringLiteral("spectrogramRestSeamGroove"));
    };
    const auto checkAt = [&](const QSize &windowSize, int viewMode) {
        window->resize(windowSize);
        QTRY_COMPARE(window->size(), windowSize);
        QVERIFY(bridge->setProperty("spectrogramViewMode", viewMode));
        const int laneCount = viewMode == 0 ? 1 : 2;
        QTRY_COMPARE(panes().size(), laneCount);
        QTRY_COMPARE(visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneLabel")).size(),
                     viewMode == 0 ? 0 : 2);
        // Each lane's lines, in the scene, once laid out for its size.
        const auto linesOf = [&](SpectrogramItem *pane) {
            QList<QRectF> lines;
            QMutexLocker lock(&pane->m_stateMutex);
            if (!pane->m_restGrid.valid || pane->m_restGrid.size.width() != qRound(pane->width() * pane->m_canvasScale)) {
                return lines;
            }
            const QRectF rect = sceneRectOf(pane);
            const double scale = pane->m_canvasScale;
            for (const QRect &line : std::as_const(pane->m_restGrid.lines)) {
                lines.push_back(QRectF(rect.left() + line.x() / scale, rect.top() + line.y() / scale,
                                       line.width() / scale, line.height() / scale));
            }
            return lines;
        };
        // Laid out for the window's size, and drawn.
        for (SpectrogramItem *pane : panes()) {
            QTRY_VERIFY(!linesOf(pane).isEmpty());
        }
        QTRY_VERIFY(sceneRectOf(spectrogram).right() <= windowSize.width());
        QTest::qWait(50);
        QQuickItem *modeSwitch = firstVisibleItemNamed(window->contentItem(), QStringLiteral("visualizationModeSwitch"));
        QVERIFY(modeSwitch != nullptr);
        QTRY_COMPARE(modeSwitch->opacity(), 1.0);
        // The plates with their 1 px edges.
        QList<QRectF> plates{sceneRectOf(modeSwitch).adjusted(-1, -1, 1, 1)};
        for (QQuickItem *label : visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneLabel"))) {
            plates.push_back(sceneRectOf(label).adjusted(-1, -1, 1, 1));
        }
        QList<QList<double>> rowsByLane;
        for (SpectrogramItem *pane : panes()) {
            const QRectF lane = sceneRectOf(pane);
            const QList<QRectF> lines = linesOf(pane);
            QVERIFY(!lines.isEmpty() && lines.size() <= (viewMode == 0 ? 4 : 3));
            QList<double> rows;
            for (const QRectF &line : lines) {
                QCOMPARE(line.left(), lane.left());
                QCOMPARE(line.right(), lane.right());
                for (const QRectF &plate : std::as_const(plates)) {
                    const bool inLane = plate.top() >= lane.top() - 4 && plate.top() < lane.bottom();
                    QVERIFY2(!inLane || line.top() >= plate.bottom() + 6,
                             qPrintable(QStringLiteral("a line at y %1 beside a plate to %2")
                                            .arg(line.top()).arg(plate.bottom())));
                }
                rows.push_back(line.top() - lane.top());
            }
            // Evenly spaced, the lane's foot and top the bands' ends.
            for (qsizetype index = 0; index < rows.size(); ++index) {
                QVERIFY2(std::abs(rows.at(index) - lane.height() * (index + 1) / (rows.size() + 1)) <= 1.0,
                         qPrintable(QStringLiteral("line %1 of %2 at %3 in %4 px")
                                        .arg(index).arg(rows.size()).arg(rows.at(index)).arg(lane.height())));
            }
            if (windowSize == QSize(1600, 1000) && pointSize <= 0) {
                QCOMPARE(rows.size(), viewMode == 0 ? 4 : 3);
            }
            // Mixed, none at the pane's middle, where "Analysing…" is
            // centred (the middle one of three struck through it).
            if (viewMode == 0) {
                for (const QRectF &line : lines) {
                    QVERIFY2(std::abs(line.center().y() - lane.center().y()) >= 12.0,
                             qPrintable(QStringLiteral("a line at y %1 in the middle").arg(line.top())));
                }
            }
            rowsByLane.push_back(rows);
        }
        // Each lane alike.
        for (const QList<double> &rows : std::as_const(rowsByLane)) {
            QCOMPARE(rows.size(), rowsByLane.first().size());
            for (qsizetype index = 0; index < rows.size(); ++index) {
                // The lanes' own heights differ by up to 2 px (PlotLanes).
                QVERIFY2(std::abs(rows.at(index) - rowsByLane.first().at(index)) <= 2.0,
                         qPrintable(QStringLiteral("line %1 at %2 and at %3 in a lane")
                                        .arg(index).arg(rowsByLane.first().at(index)).arg(rows.at(index))));
            }
        }

        // The groove between the lanes at rest, per channel.
        QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
        QVERIFY(palette != nullptr);
        const QColor restSeam = palette->property("plotRestLaneSeam").value<QColor>();
        QCOMPARE(restSeam.rgb(), QColor(QStringLiteral("#0a0318")).rgb());
        if (viewMode == 0) {
            QCOMPARE(grooves().size(), 0);
            return;
        }
        QTRY_COMPARE(grooves().size(), 1);
        const QRectF upper = sceneRectOf(panes().at(0));
        const QRectF lower = sceneRectOf(panes().at(1));
        const QRectF pane = sceneRectOf(spectrogram);
        // The lanes keep their heights, with the 1 px seam between them.
        QCOMPARE(lower.top() - upper.bottom(), 1.0);
        QCOMPARE(upper.top(), pane.top());
        QCOMPARE(upper.height(), double(qRound(pane.height() / 2)));
        QCOMPARE(lower.bottom(), pane.bottom());
        const QRectF groove = sceneRectOf(grooves().first());
        QCOMPARE(groove.height(), 3.0);
        QCOMPARE(groove.left(), upper.left());
        QCOMPARE(groove.width(), upper.width());
        QCOMPARE(groove.center().y(), upper.bottom() + 0.5);
        QCOMPARE(grooves().first()->property("color").value<QColor>().rgb(), restSeam.rgb());
        // Drawn over the lanes: their rows beside the seam show it.
        QTest::qWait(50);
        const QImage image = window->grabWindow();
        QVERIFY(!image.isNull());
        const double scale = double(image.width()) / window->width();
        for (const double row : {upper.bottom() - 0.5, upper.bottom() + 0.5, lower.top() + 0.5}) {
            const QColor drawn = image.pixelColor(int(pane.center().x() * scale), int(row * scale));
            QVERIFY2(drawn.rgb() == restSeam.rgb(),
                     qPrintable(QStringLiteral("row %1: %2").arg(row).arg(drawn.name())));
        }
        const QColor beyond = image.pixelColor(int(pane.center().x() * scale), int((lower.top() + 2.5) * scale));
        QVERIFY2(beyond.rgb() != restSeam.rgb(), qPrintable(beyond.name()));
    };
    // Each in turn at another width, at which the lanes lay their lines out
    // anew: linesOf() tells stale lines by their width. Per channel last.
    const QList<std::pair<QSize, int>> layouts = pointSize > 0
        ? QList<std::pair<QSize, int>>{{QSize(1600, 1000), 1}}
        : QList<std::pair<QSize, int>>{{QSize(1000, 720), 0}, {QSize(1600, 1000), 0},
                                       {QSize(1000, 720), 1}, {QSize(1600, 1000), 1}};
    for (const auto &[windowSize, viewMode] : layouts) {
        checkAt(windowSize, viewMode);
        if (QTest::currentTestFailed()) {
            qInfo("At %dx%d, %s", windowSize.width(), windowSize.height(), viewMode == 0 ? "mixed" : "per channel");
            return;
        }
    }

    // Playing: no groove, the seam 1 px.
    QCOMPARE(grooves().size(), 1);
    setPlayingTrack(bridge);
    QTRY_COMPARE(grooves().size(), 0);
}

// The pane at rest, before any audio, is an instrument waiting: each lane
// draws its frequency scale's lines faintly, the scale's labels only while
// the scale shows; per channel it has a stereo track's lanes, with the 1 px
// seam between them at mid-height, and mixed one lane, unnamed, as a stereo
// track's would be. The first track's columns come into those lanes: a
// stereo track keeps them, with nothing laid out again. The waveform at
// rest has the same lanes.
void TransportInputSmokeTest::paneAtRestIsAStereoInstrument() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge && viewer && spectrogram && waveformSurface);
    QCOMPARE(bridge->property("currentTrackPath").toString(), QString());
    QVERIFY(bridge->setProperty("showLevelMeter", false));
    QVERIFY(bridge->setProperty("showSpectrogramScale", false));
    const auto panes = [&]() {
        QList<SpectrogramItem *> found;
        for (QQuickItem *item : itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"))) {
            found.push_back(qobject_cast<SpectrogramItem *>(item));
        }
        return found;
    };
    // Each lane's rest grid: lines, and labels only with the scale.
    const auto restProblem = [&](bool labels) -> QString {
        for (SpectrogramItem *pane : panes()) {
            QMutexLocker lock(&pane->m_stateMutex);
            if (!pane->m_restGrid.valid || pane->m_restGrid.lines.isEmpty()) {
                return QStringLiteral("a lane without its rest lines");
            }
            if (pane->m_restGrid.images.isEmpty() == labels) {
                return QStringLiteral("a lane's labels %1").arg(labels ? "missing" : "shown");
            }
        }
        return {};
    };

    // Mixed: one lane, unnamed, with its lines.
    QVERIFY(bridge->setProperty("spectrogramViewMode", 0));
    QTRY_COMPARE(panes().size(), 1);
    QCOMPARE(spectrogram->property("laneCount").toInt(), 1);
    QTRY_VERIFY2(restProblem(false).isEmpty(), qPrintable(restProblem(false)));
    QCOMPARE(visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneHead")).size(), 0);

    // Per channel: a stereo track's two lanes, L and R, with the seam
    // between them at mid-height, each with its lines.
    QVERIFY(bridge->setProperty("spectrogramViewMode", 1));
    QTRY_COMPARE(panes().size(), 2);
    QCOMPARE(spectrogram->property("laneLabels").toStringList(),
             (QStringList{QStringLiteral("L"), QStringLiteral("R")}));
    QTRY_COMPARE(visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneHead")).size(), 2);
    QVERIFY(visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneButtons")).isEmpty());
    const QRectF upper = sceneRectOf(panes().at(0));
    const QRectF lower = sceneRectOf(panes().at(1));
    const QRectF pane = sceneRectOf(spectrogram);
    QCOMPARE(lower.top() - upper.bottom(), 1.0);
    QVERIFY(std::abs((upper.bottom() + lower.top()) / 2 - pane.center().y()) <= 1.0);
    QQuickItem *seams = itemsNamed(spectrogram, QStringLiteral("spectrogramLaneSeams")).value(0);
    QVERIFY(seams != nullptr && seams->isVisible());
    // The seam at rest in the floor's violet, over the rest lines; playing,
    // the neutral one.
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(palette != nullptr);
    QObject *spectrogramPlot = qvariant_cast<QObject *>(palette->property("spectrogramPlot"));
    QCOMPARE(seams->property("color").value<QColor>(), palette->property("plotRestLaneSeam").value<QColor>());
    QTRY_VERIFY2(restProblem(false).isEmpty(), qPrintable(restProblem(false)));
    // As drawn (the lines at 1.3:1 left the never-played pane an empty
    // slab, and the seam, at 1.45:1, read as one more line): the rest lines
    // white 20 % over the floor, some 1.78:1 on it, and the seam between
    // the lanes a cut, darker than the floor, well apart from them.
    {
        const auto luminance = [](const QColor &color) {
            const auto channel = [](double value) {
                return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
        };
        const auto contrast = [&](const QColor &a, const QColor &b) {
            return (std::max(luminance(a), luminance(b)) + 0.05) / (std::min(luminance(a), luminance(b)) + 0.05);
        };
        const QColor floor = palette->property("plotFloor").value<QColor>();
        QRect line;
        {
            SpectrogramItem *pane = panes().at(0);
            QMutexLocker lock(&pane->m_stateMutex);
            QVERIFY(!pane->m_restGrid.lines.isEmpty());
            line = pane->m_restGrid.lines.constFirst();
        }
        QTest::qWait(50);
        const QImage image = window->grabWindow();
        QVERIFY(!image.isNull());
        const double scale = double(image.width()) / window->width();
        const QPointF origin = upper.topLeft() * scale;
        const QColor drawnLine = image.pixelColor(int(origin.x()) + line.center().x(), int(origin.y()) + line.top());
        const double lineContrast = contrast(drawnLine, floor);
        QVERIFY2(lineContrast >= 1.72 && lineContrast <= 1.84,
                 qPrintable(QStringLiteral("rest line %1, %2:1 on the floor").arg(drawnLine.name()).arg(lineContrast)));
        const QColor drawnSeam = image.pixelColor(int((pane.center().x()) * scale), int((upper.bottom() + 0.5) * scale));
        QCOMPARE(drawnSeam.rgb(), palette->property("plotRestLaneSeam").value<QColor>().rgb());
        QVERIFY2(luminance(drawnSeam) < luminance(floor), qPrintable(drawnSeam.name()));
        QVERIFY2(contrast(drawnSeam, drawnLine) >= 1.8,
                 qPrintable(QStringLiteral("seam %1 on the lines' %2").arg(drawnSeam.name(), drawnLine.name())));
    }
    // The scale shown: its labels too.
    QVERIFY(bridge->setProperty("showSpectrogramScale", true));
    QTRY_VERIFY2(restProblem(true).isEmpty(), qPrintable(restProblem(true)));
    QVERIFY(bridge->setProperty("showSpectrogramScale", false));
    QTRY_VERIFY2(restProblem(false).isEmpty(), qPrintable(restProblem(false)));

    // The waveform at rest: the same two lanes, named.
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveformSurface->isVisible());
    QCOMPARE(waveformSurface->property("laneCount").toInt(), 2);
    QCOMPARE(waveformSurface->property("laneLabels").toStringList(),
             (QStringList{QStringLiteral("L"), QStringLiteral("R")}));
    // Each lane a silent one: one line at its centre, faint, about 1.3:1
    // on the view, and the seam between the lanes the stronger line, 3 px
    // (with lines at half of full scale in each lane, stronger than the
    // seam, two lanes read as eight empty rows of a table).
    QVERIFY(visibleItemsNamed(waveformSurface, QStringLiteral("waveformRestLine")).isEmpty());
    auto *editor = waveformSurface->findChild<WaveformEditorItem *>(QStringLiteral("waveformEditorItem"));
    QVERIFY(editor != nullptr);
    const auto restSeams = [&]() { return visibleItemsNamed(waveformSurface, QStringLiteral("waveformRestSeam")); };
    QTRY_COMPARE(restSeams().size(), 1);
    {
        const QColor view = palette->property("view").value<QColor>();
        const QColor line = palette->property("waveformRestLine").value<QColor>();
        const auto luminance = [](const QColor &color) {
            const auto channel = [](double value) {
                return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
        };
        const double ratio = (std::max(luminance(view), luminance(line)) + 0.05)
            / (std::min(luminance(view), luminance(line)) + 0.05);
        QVERIFY2(ratio >= 1.25 && ratio <= 1.45, qPrintable(QString::number(ratio)));
        QCOMPARE(editor->centerLineColor(), line);
        const QColor seam = palette->property("seam").value<QColor>();
        QCOMPARE(editor->laneSeamColor(), seam);
        const double seamRatio = (std::max(luminance(view), luminance(seam)) + 0.05)
            / (std::min(luminance(view), luminance(seam)) + 0.05);
        QVERIFY2(seamRatio > ratio, qPrintable(QStringLiteral("seam %1, line %2").arg(seamRatio).arg(ratio)));
        const QRectF plot = sceneRectOf(waveformSurface);
        const QRectF groove = sceneRectOf(restSeams().first());
        QCOMPARE(groove.height(), 3.0);
        QCOMPARE(groove.width(), plot.width());
        QVERIFY(std::abs(groove.center().y() - plot.center().y()) <= 1.0);
        // As drawn down the plot's middle column: in each lane one line, at
        // its centre, and the seam's three rows.
        QTRY_COMPARE(waveformSurface->opacity(), 1.0);
        QTest::qWait(100);
        const QImage image = window->grabWindow();
        const double scale = double(image.width()) / window->width();
        QList<int> inked;
        for (int y = qRound(plot.top()) + 1; y < qRound(plot.bottom()) - 1; ++y) {
            const QColor pixel = image.pixelColor(int(plot.center().x() * scale), int((y + 0.5) * scale));
            if (std::abs(pixel.red() - view.red()) + std::abs(pixel.green() - view.green())
                    + std::abs(pixel.blue() - view.blue()) > 6) {
                inked.push_back(y);
            }
        }
        QVERIFY2(inked.size() == 5 && std::abs(inked.first() - (plot.top() + plot.height() / 4)) <= 1.5
                     && std::abs(inked.last() - (plot.top() + plot.height() * 3 / 4)) <= 1.5,
                 qPrintable(QStringLiteral("%1 rows drawn").arg(inked.size())));
    }
    viewer->setProperty("visualizationMode", 0);
    QTRY_VERIFY(spectrogram->isVisible() && spectrogram->opacity() == 1.0);

    // A stereo track: its columns come into the lanes at rest, which stay.
    const QList<SpectrogramItem *> before = panes();
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackChannels", 2);
    QVERIFY(QMetaObject::invokeMethod(bridge, "precomputedSpectrogramChannelsReady",
                                      Q_ARG(int, 2), Q_ARG(bool, true)));
    QCOMPARE(panes(), before);
    QTRY_COMPARE(seams->property("color").value<QColor>(), spectrogramPlot->property("plotLaneSeam").value<QColor>());
    QCOMPARE(sceneRectOf(panes().at(0)), upper);
    QCOMPARE(sceneRectOf(panes().at(1)), lower);
    QTRY_VERIFY(!visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneButtons")).isEmpty()
                || bridge->property("channelButtonsVisibility").toInt() == 0);
    // The waveform's lanes at rest gone with the first track.
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveformSurface->isVisible());
    QTRY_VERIFY(restSeams().isEmpty());
    QVERIFY(editor->centerLineColor() != palette->property("waveformRestLine").value<QColor>());
}

// The crosshair and its readout give way to the plot's plates: on a lane's
// label or its buttons, or on the mode switch, they fade out over
// shortDuration, and come back off them (its line ran under the plates and
// through them), in both modes.
void TransportInputSmokeTest::crosshairGivesWayToThePlotsPlates() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge && viewer && layer && spectrogramSurface && waveformSurface);
    bridge->setProperty("showSpectrogramCrosshair", true);
    bridge->setProperty("spectrogramViewMode", 1);
    QQuickItem *modeSwitch = firstVisibleItemNamed(window->contentItem(), QStringLiteral("visualizationModeSwitch"));
    QVERIFY(modeSwitch != nullptr);
    QTRY_VERIFY(!visibleItemsNamed(layer, QStringLiteral("plotLaneLabel")).isEmpty());
    QQuickItem *label = visibleItemsNamed(layer, QStringLiteral("plotLaneLabel")).first();
    const auto opacities = [&](int mode) {
        if (mode == 0) {
            QList<double> values;
            for (QQuickItem *pane : visibleItemsNamed(spectrogramSurface, QStringLiteral("spectrogramPaneItem"))) {
                values.push_back(pane->property("crosshairOpacity").toDouble());
            }
            return values;
        }
        auto *waveform = waveformSurface->property("waveformItem").value<QObject *>();
        return QList<double>{waveform->property("crosshairOpacity").toDouble()};
    };
    const auto all = [&](int mode, double value) {
        const QList<double> values = opacities(mode);
        return !values.isEmpty()
            && std::all_of(values.begin(), values.end(), [&](double each) { return std::abs(each - value) < 1e-6; });
    };
    // The fades in animation time, each to its end.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
    for (const int mode : {0, 1}) {
        viewer->setProperty("visualizationMode", mode);
        settle();
        QVERIFY((mode == 0 ? spectrogramSurface : waveformSurface)->isVisible());
        const QPoint onPlot = sceneRectOf(layer).center().toPoint();
        QTest::mouseMove(window, onPlot);
        settle();
        QVERIFY(all(mode, 1.0));
        QCOMPARE(layer->property("crosshairLevel").toDouble(), 1.0);
        for (QQuickItem *plate : {label, modeSwitch}) {
            QTest::mouseMove(window, sceneRectOf(plate).center().toPoint());
            QTRY_VERIFY(layer->property("plateHovered").toBool());
            settle();
            QVERIFY2(all(mode, 0.0), qPrintable(QStringLiteral("mode %1 on %2").arg(mode).arg(plate->objectName())));
            QTest::mouseMove(window, onPlot);
            QTRY_VERIFY(!layer->property("plateHovered").toBool());
            settle();
            QVERIFY(all(mode, 1.0));
        }
    }
    // Over shortDuration, OutCubic: under way after a frame.
    QTest::mouseMove(window, sceneRectOf(modeSwitch).center().toPoint());
    QTRY_VERIFY(layer->property("plateHovered").toBool());
    QVERIFY(animations.step(16));
    const double during = layer->property("crosshairLevel").toDouble();
    QVERIFY2(during > 0.0 && during < 1.0, qPrintable(QString::number(during)));
    settle();
    QCOMPARE(layer->property("crosshairLevel").toDouble(), 0.0);
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
        QStringLiteral("spectrogramPopupControls"));
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

        // A 38 px band on the plot's strip colour, opaque, with its edge
        // under it, over the top of the view: 32 px grown with its text, a
        // step larger.
        QObject *plotColors = qvariant_cast<QObject *>(palette->property(shown.plotColors));
        QVERIFY(plotColors != nullptr);
        QQuickItem *strip = visibleItemNamed(fullscreen, shown.stripName);
        QVERIFY2(strip != nullptr, name.constData());
        QTRY_COMPARE(sceneRectOf(strip), QRectF(0, 0, fullscreen->width(), 39));
        QCOMPARE(colorOf(strip, "color"), colorOf(plotColors, "plotStrip"));
        QCOMPARE(qAlpha(colorOf(strip, "color")), 255);
        QQuickItem *hairline = itemNamed(strip, QStringLiteral("viewerStripHairline"));
        QVERIFY(hairline != nullptr);
        const qreal band = strip->height() - 1;
        QCOMPARE(sceneRectOf(hairline), QRectF(0, band, fullscreen->width(), 1));
        QCOMPARE(colorOf(hairline, "color"), colorOf(plotColors, "plotPlateEdge"));

        // The title semibold, then "Artist — Album" and the time, secondary.
        QQuickItem *title = itemNamed(strip, QStringLiteral("viewerStripTitle"));
        QQuickItem *context = itemNamed(strip, QStringLiteral("viewerStripContext"));
        QQuickItem *time = itemNamed(strip, QStringLiteral("viewerStripTime"));
        QVERIFY(title != nullptr && context != nullptr && time != nullptr);
        QCOMPARE(title->property("text").toString(), QStringLiteral("Morning Tide Field"));
        QCOMPARE(qvariant_cast<QFont>(title->property("font")).weight(), QFont::DemiBold);
        // Its text on the type scale of the viewer's text over the whole
        // screen, 1.2 times the window's, as the album art's details are
        // (the strip was at the window's size over them).
        const qreal basePointSize = palette->property("basePointSize").toReal();
        for (QQuickItem *label : {title, context, time}) {
            QCOMPARE(qvariant_cast<QFont>(label->property("font")).pointSizeF(), basePointSize * 1.2);
        }
        // 16 px in from the view's edge, and 14 px, 12 px a step larger, to
        // "Artist — Album".
        QTRY_COMPARE(title->mapToScene(QPointF(0, 0)).x(), 16.0);
        QCOMPARE(context->mapToScene(QPointF(0, 0)).x() - title->mapToScene(QPointF(title->width(), 0)).x(), 14.0);
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

        // A flat 30 px Close at the right, on the strip's type, centred in
        // the band, 8 px in (at 4 px its focus ring met the screen's
        // corner), in the plot's colours; the view fills the window under
        // the strip.
        QQuickItem *close = visibleItemNamed(fullscreen, shown.closeName);
        QVERIFY2(close != nullptr, name.constData());
        QVERIFY(strip->isAncestorOf(close));
        QCOMPARE(sceneRectOf(close).size(), QSizeF(30, 30));
        QCOMPARE(sceneRectOf(close).right(), fullscreen->width() - 8.0);
        QCOMPARE(sceneRectOf(close).center().y(), band / 2);
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

// The strip is as tall as its line of text and 4 px above and below it,
// 32 px and its edge at the least: at 24 pt its text ran 6 px over the top
// of the screen. It still lies over the view, which keeps the whole
// window, and what keeps below it moves down with it. Its size with the
// default font: viewersShowTheTrackInATitleStripOverTheWholeScreen.
void TransportInputSmokeTest::viewerStripHoldsItsTextAtALargeFont() {
    constexpr qreal pointSize = 24;
    const ScopedApplicationFont font(pointSize);
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
            QCOMPARE(qvariant_cast<QFont>(label->property("font")).pointSizeF(), pointSize * 1.2);
            line = std::max(line, label->implicitHeight());
            labels << label;
        }
        const qreal band = std::max({38.0, std::ceil(line) + 8.0, close->height() + 8.0});
        QTRY_COMPARE(sceneRectOf(strip), QRectF(0, 0, fullscreen->width(), band + 1.0));
        QVERIFY2(band > 38.0, name.constData());
        // Each text whole inside the band, over the edge under it.
        for (QQuickItem *label : std::as_const(labels)) {
            const QRectF rect = sceneRectOf(label);
            QVERIFY2(rect.top() >= 0.0 && rect.bottom() <= band,
                     qPrintable(QStringLiteral("%1: %2 from %3 to %4 in a %5 px band")
                                    .arg(QString::fromUtf8(name), label->objectName())
                                    .arg(rect.top()).arg(rect.bottom()).arg(band)));
        }
        // Close, centred in the band, 8 px in from the right.
        QCOMPARE(sceneRectOf(close).right(), fullscreen->width() - 8.0);
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

// The album art's details start 12 px below the viewer's controls, which
// grow with the font: in the main window, under the plate in its corner;
// over the whole screen, under the title strip. At 24 pt the file's name
// lay 8 px under the plate's buttons.
void TransportInputSmokeTest::albumArtDetailsKeepBelowTheViewerControls() {
    const ScopedApplicationFont font(24);
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
    QTRY_COMPARE(albumArt->property("infoTopInset").toReal(), plateBottom);
    QTRY_VERIFY2(std::abs(sceneRectOf(title).top() - (plateBottom + 12)) <= 0.5,
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
    QTRY_VERIFY2(std::abs(sceneRectOf(title).top() - (sceneRectOf(strip).bottom() + 12)) <= 0.5,
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
        QCOMPARE(sceneRectOf(strip), QRectF(0, 0, fullscreen->width(), strip->height()));
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

    // In the main window, the album art viewer's corner plate hides while
    // the pointer rests and comes back as it moves, as the visualization
    // viewer's does (it never hid).
    QObject *albumArtAutoHide =
        window->findChild<QObject *>(QStringLiteral("albumArtFullscreenControlsAutoHide"));
    QVERIFY(albumArtAutoHide != nullptr);
    bridge->setProperty("viewerFullscreenMode", 0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QVERIFY(albumArtAutoHide->property("active").toBool());
    QQuickItem *popupClose = visibleItemNamed(window, QStringLiteral("albumArtPopupCloseButton"));
    QVERIFY(popupClose != nullptr);
    const QRectF popupCloseRect = sceneRectOf(popupClose);
    albumArtAutoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!popupClose->isVisible());
    albumArtAutoHide->setProperty("hideDelay", 600000);
    QTest::mouseMove(window, QPoint(window->width() / 3, window->height() / 2));
    QTest::mouseMove(window, QPoint(window->width() / 3 + 5, window->height() / 2));
    QTRY_VERIFY(popupClose->isVisible());
    QCOMPARE(sceneRectOf(popupClose), popupCloseRect);
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

    // The pane's: the mode switch and the first lane's head's plates 5 px
    // from the top, the label centred in the head's plate.
    const auto headPlatesTop = [](QQuickItem *laneHead) {
        return sceneRectOf(laneHead).top();
    };
    const auto controlsTop = [&](QQuickItem *in) {
        // The plot's in the pane; in the window's viewer, its corner plate.
        QQuickItem *modeSwitch = firstVisibleItemNamed(in, QStringLiteral("visualizationModeSwitch"));
        if (modeSwitch == nullptr) {
            modeSwitch = firstVisibleItemNamed(in, QStringLiteral("spectrogramPopupControls"));
        }
        QQuickItem *laneHead = firstVisibleItemNamed(in, QStringLiteral("plotLaneHead"));
        return std::make_pair(modeSwitch ? sceneRectOf(modeSwitch).top() : -1.0,
                              laneHead ? headPlatesTop(laneHead) : -1.0);
    };
    QTRY_COMPARE(controlsTop(window->contentItem()),
                 std::make_pair(sceneRectOf(surface).top() + 5.0, sceneRectOf(surface).top() + 5.0));

    // The viewer in the main window has no strip: the same.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QTRY_COMPARE(controlsTop(window->contentItem()), std::make_pair(5.0, 5.0));
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(!popup->property("visible").toBool());

    // Over the whole screen, the controls that show with the strip keep
    // below it, whether it shows or not, so they never move and are never
    // under it; the time scale stays on the view's top edge, which the
    // strip leaves clear while it is hidden. The mode switch is in the
    // strip, so the plot's corner has nothing to keep clear of.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QTRY_COMPARE(sceneRectOf(surface).top(), 0.0);
    const qreal belowStrip = strip->height() + 5.0;
    QTRY_COMPARE(controlsTop(fullscreen->contentItem()), std::make_pair(-1.0, belowStrip));
    auto *spectrogram = qobject_cast<SpectrogramItem *>(
        firstVisibleItemNamed(surface, QStringLiteral("spectrogramPaneItem")));
    QVERIFY(spectrogram != nullptr);
    QVERIFY(spectrogram->showTimeLabels());
    QCOMPARE(sceneRectOf(spectrogram).top(), 0.0);
    QTRY_COMPARE(spectrogram->reservedCorner(), QSizeF(0, 0));
    for (const int mode : {1, 0}) {
        viewer->setProperty("visualizationMode", mode);
        QTRY_COMPARE(viewer->property("visualizationMode").toInt(), mode);
        QQuickItem *modeSwitch =
            firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("spectrogramStripModeSwitch"));
        QVERIFY(modeSwitch != nullptr);
        QTRY_VERIFY(sceneRectOf(strip).contains(sceneRectOf(modeSwitch)));
    }
    QTRY_COMPARE(controlsTop(fullscreen->contentItem()), std::make_pair(-1.0, belowStrip));
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTRY_VERIFY(!strip->isVisible());
    QQuickItem *laneHead = firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("plotLaneHead"));
    QVERIFY(laneHead != nullptr);
    QCOMPARE(headPlatesTop(laneHead), belowStrip);
    autoHide->setProperty("hideDelay", 5000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());

    // Back in the pane, as before.
    QTRY_COMPARE(controlsTop(window->contentItem()),
                 std::make_pair(sceneRectOf(surface).top() + 5.0, sceneRectOf(surface).top() + 5.0));
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
        QCOMPARE(sceneRectOf(controls).right(), window->width() - 5.0);
        QCOMPARE(sceneRectOf(controls).top(), 5.0);
        QQuickItem *close = visibleItemNamed(window, shown.closeName);
        QVERIFY(close != nullptr && controls->isAncestorOf(close));
        QCOMPARE(sceneRectOf(close).size(), QSizeF(24, 24));

        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(close));
        QTRY_VERIFY2(!viewer->property(shown.openProperty).toBool(), name.constData());
        QTRY_COMPARE(window->title(), mainTitle);
    }
}

// The title strip over the whole screen is opaque (the playhead and the
// spectrogram showed through it): a line the view draws the whole height,
// as the playhead and the crosshair are, ends at the strip's lower edge.
// The strip still draws over the view, which keeps its size.
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
        // between the strip's text and its buttons, clear of both.
        QQuickItem *stripTrack = itemNamed(strip, QStringLiteral("viewerStripTrack"));
        QQuickItem *stripControls = itemNamed(strip, QStringLiteral("viewerStripControls"));
        QVERIFY(stripTrack != nullptr && stripControls != nullptr);
        QTRY_VERIFY(sceneRectOf(stripControls).left() - sceneRectOf(stripTrack).right() > 8);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nRectangle { width: 1; color: \"#ff00ff\" }", QUrl());
        std::unique_ptr<QQuickItem> line(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(line != nullptr, qPrintable(component.errorString()));
        line->setParentItem(host);
        line->setZ(100);
        line->setX(std::round((sceneRectOf(stripTrack).right() + sceneRectOf(stripControls).left()) / 2));
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
        for (int y = 1; y < strip->height() - 1; y += 3) {
            const QColor at = pixel(x, y);
            QVERIFY2(std::abs(at.red() - stripColor.red()) <= 1 && std::abs(at.green() - stripColor.green()) <= 1
                         && std::abs(at.blue() - stripColor.blue()) <= 1,
                     qPrintable(QStringLiteral("%1: %2 at y %3, the strip %4")
                                    .arg(QLatin1String(where), at.name(), QString::number(y), stripColor.name())));
        }
        // Below it, the line shows.
        QCOMPARE(pixel(x, strip->height() + 20).name(), QStringLiteral("#ff00ff"));
        line.reset();
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

// The pointer on the viewer's own controls is on the view (in the viewer in
// the window, the mode switch went as the pointer reached the corner plate
// beside it): the switch stays, in the window on the corner plate and its
// Close, and over the whole screen on the title strip. It shows with the
// viewer's controls, which it hides with.
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
        // The switch is in the viewer's controls, and not on the plot.
        QQuickItem *shownSwitch = itemNamed(controls, fullscreenMode == 1
                                                          ? QStringLiteral("spectrogramStripModeSwitch")
                                                          : QStringLiteral("spectrogramPopupModeSwitch"));
        QVERIFY(shownSwitch != nullptr);
        // The pointer on the view reveals the switch.
        QTest::mouseMove(host, QPoint(host->width() / 2, host->height() / 2));
        QTRY_COMPARE(controls->opacity(), 1.0);
        for (QQuickItem *spot : spots) {
            QVERIFY2(spot != nullptr, where.constData());
            const QPoint at = fullscreenMode == 1 && spot == controls
                ? QPoint(host->width() / 2, int(controls->height() / 2))
                : centerInWindow(spot);
            // Longer than the switch's fade, in animation time: it stays
            // revealed.
            SteppedAnimationDriver animations;
            QTest::mouseMove(host, at);
            QTRY_VERIFY2(controls->property("hovered").toBool(), where.constData());
            (void)animations.step(300);
            QCOMPARE(controls->opacity(), 1.0);
            QVERIFY(shownSwitch->isVisible());
            QVERIFY(!modeSwitch->isVisible());
        }
        // Off the view and its controls, it stays while the viewer's
        // controls show, as their part, and goes with them, at their level.
        {
            SteppedAnimationDriver animations;
            QTest::mouseMove(host, QPoint(-20, -20));
            (void)animations.step(300);
        }
        QVERIFY(shownSwitch->isVisible());
        QCOMPARE(controls->opacity(), 1.0);
        QObject *autoHide = window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
        QVERIFY(autoHide != nullptr);
        autoHide->setProperty("hideDelay", 1);
        QTRY_VERIFY2(!shownSwitch->isVisible(), where.constData());
        QCOMPARE(controls->opacity(), 0.0);
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

    // One order everywhere: [Zoom ↺] [mode] [✕], in both viewers as in the
    // main pane. In a viewer, the zoom and the mode switch are its own
    // controls', before Close, on the corner plate in the window and in the
    // title strip over the whole screen, and not on the plot.
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
        // The window has its size: the controls end 5 px from its right.
        QTRY_COMPARE(sceneRectOf(controls).right(),
                     fullscreenMode == 1 ? double(host->width()) : host->width() - 5.0);

        const bool inStrip = fullscreenMode == 1;
        QQuickItem *shownReadout = itemNamed(controls, QStringLiteral("viewerZoomReadout"));
        QQuickItem *shownReset = itemNamed(controls, QStringLiteral("viewerResetButton"));
        QQuickItem *shownSwitch = itemNamed(controls, inStrip ? QStringLiteral("spectrogramStripModeSwitch")
                                                              : QStringLiteral("spectrogramPopupModeSwitch"));
        QVERIFY(shownReadout != nullptr && shownReset != nullptr && shownSwitch != nullptr);
        for (const int mode : {0, 1}) {
            viewer->setProperty("visualizationMode", mode);
            // Nothing at the default zoom.
            QTRY_VERIFY(!shownReadout->isVisible());
            QVERIFY(!shownReset->isVisible());

            // Zoomed: a quiet readout and Reset.
            if (mode == 0) {
                spectrogramSurface->setProperty("_viewerZoomLevel", 2.0);
            } else {
                waveform->setZoomLevel(4.0);
            }
            QTRY_VERIFY(shownReadout->isVisible() && shownReset->isVisible());
            QCOMPARE(shownReadout->property("text").toString(),
                     mode == 0 ? QStringLiteral("Zoom 200%") : QStringLiteral("Zoom 400%"));
            // None on the plot.
            QVERIFY(!readout->isVisible() && !reset->isVisible() && !zoomPlate->isVisible() && !modeSwitch->isVisible());
            {
                // The readout, Reset, the mode switch and Close, the groups
                // set off by 1 px dividers that keep 8 px from the mode
                // switch, whose chosen segment has an edge, and the
                // buttons' 4 px from Reset and Close; Close at the strip's
                // 8 px inset or 4 px inside the corner's plate.
                QTRY_VERIFY(sceneRectOf(shownReadout).right() <= sceneRectOf(shownReset).left());
                QCOMPARE(sceneRectOf(shownSwitch).left() - sceneRectOf(shownReset).right(), 13.0);
                QCOMPARE(sceneRectOf(close).left() - sceneRectOf(shownSwitch).right(), 13.0);
                QCOMPARE(sceneRectOf(close).right(), inStrip ? host->width() - 8.0 : sceneRectOf(controls).right() - 4.0);
                for (QQuickItem *item : {shownReadout, shownReset, shownSwitch, close}) {
                    QVERIFY(sceneRectOf(controls).contains(sceneRectOf(item)));
                }
                QCOMPARE(visibleItemsNamed(controls, QStringLiteral("viewerControlsDivider")).size(), 1);
                QCOMPARE(visibleItemsNamed(controls, QStringLiteral("viewerControlsCloseDivider")).size(), 1);
            }
            // A double-click on the readout leaves the viewer open.
            QTest::mouseDClick(host, Qt::LeftButton, Qt::NoModifier, centerInWindow(shownReadout));
            QCoreApplication::processEvents();
            QVERIFY(viewer->property("spectrogramViewerOpen").toBool());
            QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, centerInWindow(shownReset));
            if (mode == 0) {
                QTRY_COMPARE(spectrogramZoom(), 1.0);
            } else {
                QTRY_COMPARE(waveform->zoomLevel(), 1.0);
            }
            QTRY_VERIFY(!shownReadout->isVisible());
            QVERIFY(!shownReset->isVisible());
            QVERIFY(viewer->property("spectrogramViewerOpen").toBool());
        }
        viewer->setProperty("visualizationMode", 0);
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

// The corner is one row: at rest, zoomed and following, the zoom's plate
// floated 68 px from the corner, beside the hidden switch's place. It
// shows and hides with the switch, on the pointer over the pane and on
// keyboard focus, and neither moves as it does.
void TransportInputSmokeTest::mainPaneCornerShowsAndHidesAsOneRow() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(bridge && spectrogramSurface && layer);
    setPlayingTrack(bridge);
    QQuickItem *plate = itemNamed(layer, QStringLiteral("plotZoomPlate"));
    QQuickItem *reset = itemNamed(layer, QStringLiteral("plotZoomResetButton"));
    QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(plate && reset && modeSwitch);
    const QPoint away(8, window->height() - 8);
    const QPoint onPane = layer->mapToScene(QPointF(layer->width() / 2, layer->height() / 2)).toPoint();

    QTest::mouseMove(window, away);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
    spectrogramSurface->setProperty("_widgetZoomLevel", 2.0);
    QTRY_VERIFY(plate->isVisible());
    QTRY_COMPARE(plate->opacity(), 0.0);

    // The wheel's pointer is on the pane: the whole row, on one curve.
    QTest::mouseMove(window, onPane);
    qreal furthest = 0.0;
    QElapsedTimer fading;
    fading.start();
    while ((plate->opacity() < 1.0 || modeSwitch->opacity() < 1.0) && fading.elapsed() < 5000) {
        QTest::qWait(4);
        furthest = std::max(furthest, std::abs(plate->opacity() - modeSwitch->opacity()));
    }
    QCOMPARE(plate->opacity(), 1.0);
    QCOMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY2(furthest < 0.05, qPrintable(QString::number(furthest)));
    const QRectF resetShown = sceneRectOf(reset);
    const QRectF switchShown = sceneRectOf(modeSwitch);

    QTest::mouseMove(window, away);
    QTRY_COMPARE(plate->opacity(), 0.0);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
    QCOMPARE(sceneRectOf(reset), resetShown);
    QCOMPARE(sceneRectOf(modeSwitch), switchShown);

    // Hidden, Reset is a Tab stop, which brings the row.
    reset->forceActiveFocus(Qt::TabFocusReason);
    QTRY_COMPARE(plate->opacity(), 1.0);
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QCOMPARE(sceneRectOf(reset), resetShown);
}

namespace {

// Whether a lane of the spectrogram draws the crosshair: the pointer's
// lane, and the others its line, from crosshairSharedX.
bool spectrogramCrosshairShown(QQuickItem *spectrogramSurface) {
    for (QQuickItem *lane : itemsNamed(spectrogramSurface, QStringLiteral("spectrogramPaneItem"))) {
        if (lane->property("crosshairEnabled").toBool()
            && (lane->property("pointerOn").toBool() || lane->property("crosshairSharedX").toDouble() >= 0.0)) {
            return true;
        }
    }
    return false;
}

} // namespace

void TransportInputSmokeTest::mainPaneCornerAndCrosshairShareOneHover_data() {
    // 0 the spectrogram, 1 the waveform; 0 mixed, 1 per channel.
    QTest::addColumn<int>("view");
    QTest::addColumn<int>("viewMode");
    QTest::newRow("spectrogram, mixed") << 0 << 0;
    QTest::newRow("spectrogram, per channel") << 0 << 1;
    QTest::newRow("waveform, mixed") << 1 << 0;
    QTest::newRow("waveform, per channel") << 1 << 1;
}

// The corner's row and the plot's crosshair are one hover: the row went on
// the lanes' hover handlers, which let go of the pointer where the plot
// kept it, such as at the seam between the waveform's lanes, while the
// crosshair stayed (H14). Both show wherever the pointer is on
// the plot, through a middle-drag, and go together as it leaves.
void TransportInputSmokeTest::mainPaneCornerAndCrosshairShareOneHover() {
    QFETCH(int, view);
    QFETCH(int, viewMode);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(bridge && viewer && spectrogramSurface && waveformSurface && layer);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(waveform && modeSwitch);
    setPlayingTrack(bridge);
    // The plot out to the pane's right edge.
    QVERIFY(bridge->setProperty("showLevelMeter", false));
    bridge->setProperty("showSpectrogramCrosshair", true);
    bridge->setProperty("spectrogramDisplayMode", 1);
    bridge->setProperty("spectrogramViewMode", viewMode);
    setChannelCount(bridge, spectrogramSurface, viewMode == 1 ? 2 : 1);
    QQuickItem *surface = view == 1 ? waveformSurface : spectrogramSurface;
    if (view == 1) {
        viewer->setProperty("visualizationMode", 1);
        QTRY_VERIFY(waveform->isVisible());
        QTRY_COMPARE(waveformSurface->property("laneCount").toInt(), viewMode == 1 ? 2 : 1);
        waveform->setZoomLevel(4.0);
    } else {
        QTRY_COMPARE(spectrogramSurface->property("laneCount").toInt(), viewMode == 1 ? 2 : 1);
        spectrogramSurface->setProperty("_widgetZoomLevel", 4.0);
    }
    QTRY_VERIFY(waveform->crosshairEnabled() || view == 0);

    // The crosshair the plot draws: for the waveform, what it paints at the
    // pointer that it paints without one; for the spectrogram, its lanes'.
    const auto crosshairShown = [&](QPointF scene) {
        if (view == 0) {
            return spectrogramCrosshairShown(spectrogramSurface);
        }
        const QSize size(int(waveform->width()), int(waveform->height()));
        const auto painted = [&]() {
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            waveform->paint(&painter);
            painter.end();
            return image;
        };
        const QImage with = painted();
        waveform->setCrosshairEnabled(false);
        const QImage without = painted();
        waveform->setCrosshairEnabled(true);
        const int column = std::clamp(int(waveform->mapFromScene(scene).x()), 0, size.width() - 1);
        for (int y = 0; y < size.height(); ++y) {
            if (with.pixel(column, y) != without.pixel(column, y)) {
                return true;
            }
        }
        return false;
    };
    // The pointer where a test cannot put it, off whole pixels.
    const auto moveTo = [&](QPointF scene, Qt::MouseButtons buttons = Qt::NoButton) {
        QMouseEvent move(QEvent::MouseMove, scene, scene, window->mapToGlobal(scene), Qt::NoButton, buttons,
                         Qt::NoModifier);
        QGuiApplication::sendEvent(window, &move);
    };
    const auto agree = [&](QPointF scene, bool on) -> QString {
        // The row's pane term: a view moved by the drag keeps the row at rest.
        const bool corner = layer->property("cornerWanted").toBool();
        const bool crosshair = crosshairShown(scene);
        if (corner != on || crosshair != on) {
            return QStringLiteral("at %1, %2: the corner %3, the crosshair %4")
                .arg(scene.x()).arg(scene.y()).arg(corner ? "shown" : "hidden").arg(crosshair ? "shown" : "hidden");
        }
        return {};
    };
    const QRectF plot = sceneRectOf(view == 1 ? static_cast<QQuickItem *>(waveform) : surface);
    QVERIFY2(qAbs(plot.right() - window->width()) < 1.0,
             qPrintable(QStringLiteral("%1 in %2").arg(plot.right()).arg(window->width())));
    const QPointF away(8, window->height() - 8);
    moveTo(away);
    QTRY_VERIFY(!modeSwitch->property("revealed").toBool());
    QVERIFY(!layer->property("cornerWanted").toBool());

    const double middleY = plot.top() + std::floor(plot.height() / 4);
    QList<QPointF> onPlot{QPointF(plot.center().x(), middleY), QPointF(plot.left() + 0.5, middleY),
                          QPointF(plot.right() - 0.4, middleY), QPointF(plot.right() - 0.4, plot.bottom() - 2.5)};
    if (view == 1 && viewMode == 1) {
        // The seam between the waveform's lanes, which is neither lane's
        // but the plot item's, and its crosshair's.
        onPlot.append(QPointF(plot.center().x(), plot.top() + std::round(plot.height() / 2) + 0.25));
    }
    for (const QPointF &at : onPlot) {
        moveTo(at);
        QTRY_VERIFY2(agree(at, true).isEmpty(), qPrintable(agree(at, true)));
        moveTo(away);
        QTRY_VERIFY2(agree(away, false).isEmpty(), qPrintable(agree(away, false)));
    }

    // A middle-drag keeps both, out past the plot's edge, and lets them go
    // as it ends there.
    const QPointF from(plot.center().x(), middleY);
    moveTo(from);
    QTRY_VERIFY2(agree(from, true).isEmpty(), qPrintable(agree(from, true)));
    QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, from.toPoint());
    QPointF at = from.toPoint();
    for (int step = 0; step < 12; ++step) {
        at.rx() -= 30;
        QTest::mouseMove(window, at.toPoint());
        QVERIFY2(agree(at, true).isEmpty(), qPrintable(QStringLiteral("step %1: ").arg(step) + agree(at, true)));
    }
    at = QPointF(at.x(), plot.top() - 4);
    QTest::mouseMove(window, at.toPoint());
    QVERIFY2(agree(at, true).isEmpty(), qPrintable(QStringLiteral("past the edge: ") + agree(at, true)));
    QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, at.toPoint());
    QTRY_VERIFY2(agree(at, false).isEmpty(), qPrintable(QStringLiteral("let go: ") + agree(at, false)));
}

void TransportInputSmokeTest::laneSeamIsOnThePlot_data() {
    // 0 the spectrogram, 1 the waveform.
    QTest::addColumn<int>("view");
    QTest::newRow("spectrogram") << 0;
    QTest::newRow("waveform") << 1;
}

// Per channel, the 1 px seam between two lanes is neither lane's, but it is
// on the plot: the corner's row and the crosshair stay there, rather than
// go for a frame as the pointer crosses it (the lanes' hover is -1 there,
// and the spectrogram's lanes are apart).
void TransportInputSmokeTest::laneSeamIsOnThePlot() {
    QFETCH(int, view);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(bridge && viewer && spectrogramSurface && waveformSurface && layer);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("showSpectrogramCrosshair", true);
    bridge->setProperty("spectrogramViewMode", 1);
    setChannelCount(bridge, spectrogramSurface, 2);
    QQuickItem *surface = view == 1 ? waveformSurface : spectrogramSurface;
    if (view == 1) {
        viewer->setProperty("visualizationMode", 1);
        QTRY_VERIFY(waveform->isVisible());
        QTRY_VERIFY(waveform->crosshairEnabled());
    }
    QTRY_COMPARE(surface->property("laneCount").toInt(), 2);
    const auto crosshairShown = [&](QPointF scene) {
        if (view == 0) {
            return spectrogramCrosshairShown(spectrogramSurface);
        }
        // What the waveform paints at the pointer that it paints without one.
        const QSize size(int(waveform->width()), int(waveform->height()));
        const auto painted = [&]() {
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            waveform->paint(&painter);
            painter.end();
            return image;
        };
        const QImage with = painted();
        waveform->setCrosshairEnabled(false);
        const QImage without = painted();
        waveform->setCrosshairEnabled(true);
        const int column = std::clamp(int(waveform->mapFromScene(scene).x()), 0, size.width() - 1);
        for (int y = 0; y < size.height(); ++y) {
            if (with.pixel(column, y) != without.pixel(column, y)) {
                return true;
            }
        }
        return false;
    };
    const auto agree = [&](QPointF scene, bool on) -> QString {
        const bool corner = layer->property("cornerWanted").toBool();
        const bool crosshair = crosshairShown(scene);
        if (corner != on || crosshair != on) {
            return QStringLiteral("at %1, %2: the corner %3, the crosshair %4")
                .arg(scene.x()).arg(scene.y()).arg(corner ? "shown" : "hidden").arg(crosshair ? "shown" : "hidden");
        }
        return {};
    };
    const QRectF plot = sceneRectOf(surface);
    // The seam's row: the first lane ends on it, the second starts after it.
    const int seamRow = int(std::round(plot.height() / 2));
    const QPointF away(8, window->height() - 8);
    const QPointF above(plot.center().x(), plot.top() + seamRow - 6);
    const QPointF seam(plot.center().x() + 3, plot.top() + seamRow);
    const QPointF below(plot.center().x() + 6, plot.top() + seamRow + 6);
    QTest::mouseMove(window, away.toPoint());
    QTRY_VERIFY2(agree(away, false).isEmpty(), qPrintable(agree(away, false)));
    for (const QPointF &at : {above, seam, below, seam}) {
        QTest::mouseMove(window, at.toPoint());
        QTRY_VERIFY2(agree(at, true).isEmpty(), qPrintable(agree(at, true)));
    }
    if (view == 0) {
        // Through the seam, the lanes' line is at the pointer.
        for (QQuickItem *lane : itemsNamed(spectrogramSurface, QStringLiteral("spectrogramPaneItem"))) {
            QCOMPARE(lane->property("crosshairSharedX").toDouble(), lane->mapFromScene(seam.toPoint()).x());
        }
    }
    QTest::mouseMove(window, away.toPoint());
    QTRY_VERIFY2(agree(away, false).isEmpty(), qPrintable(agree(away, false)));
}

// The main pane shows its zoom while it is zoomed (with the scale off,
// nothing showed it): the viewers' readout and Reset, on a plate left of
// the mode switch, their edges 6 px apart, in its place also while both
// hide. The plot keeps its place and size; its overlays keep clear of the
// plate. A viewer shows the same readout and Reset on its own plate, in the
// same order.
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
    // The corner's row hidden, as the pointer is away from the pane; the
    // pointer comes back below.
    QTest::mouseMove(window, QPoint(8, window->height() - 8));
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
        // In its place at once, shown as the switch is, with the viewers'
        // wording, in the plot's colours.
        QVERIFY(plate->isVisible());
        QTRY_COMPARE(plate->opacity(), modeSwitch->opacity());
        QVERIFY(readout->isVisible() && reset->isVisible());
        QCOMPARE(readout->property("text").toString(),
                 mode == 0 ? QStringLiteral("Zoom 244%") : QStringLiteral("Zoom 400%"));
        QCOMPARE(reset->property("text").toString(), QStringLiteral("Reset Zoom"));
        QCOMPARE(plate->property("plotColors").value<QObject *>(),
                 layer->property("plotColors").value<QObject *>());
        // Left of the switch, their 1 px edges outside them 6 px apart, on
        // its top, also while the switch hides
        // (the click on Reset below puts the pointer on the pane, which
        // reveals it).
        if (mode == 0) {
            QCOMPARE(modeSwitch->opacity(), 0.0);
        }
        QTRY_COMPARE(sceneRectOf(plate).right() + 8, sceneRectOf(modeSwitch).left());
        QCOMPARE(sceneRectOf(plate).top(), sceneRectOf(modeSwitch).top());
        QCOMPARE(sceneRectOf(plate).height(), sceneRectOf(modeSwitch).height());
        QVERIFY(sceneRectOf(readout).right() <= sceneRectOf(reset).left());
        // The plot stays where it is; its overlays keep clear of the plate.
        QCOMPARE(sceneRectOf(spectrogramSurface), plotBefore);
        const QSizeF corner = surface->property("reservedCorner").toSizeF();
        QCOMPARE(corner.width(), cornerBefore.width() + 8 + plate->width());
        QCOMPARE(corner.height(), cornerBefore.height());

        // Reset: the default zoom, and the plate goes.
        QTest::mouseMove(window, centerInWindow(reset));
        QTRY_COMPARE(plate->opacity(), 1.0);
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

    // In a viewer, none of the plot's: the viewer's one plate has the same
    // readout and Reset, then the modes, then Close.
    viewer->setProperty("visualizationMode", 0);
    bridge->setProperty("viewerFullscreenMode", 0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(spectrogramSurface->property("viewerMode").toBool());
    spectrogramSurface->setProperty("_viewerZoomLevel", 2.0);
    QQuickItem *controls = visibleItemNamed(window, QStringLiteral("spectrogramPopupControls"));
    QVERIFY(controls != nullptr);
    QTRY_VERIFY(!plate->isVisible() && !modeSwitch->isVisible());

    // One plate in the windowed viewer's corner (a row of three plates read
    // as unlike controls), in the plot plate's fill and edge and as tall as
    // it, with two dividers.
    QQuickItem *cornerPlate = itemNamed(controls, QStringLiteral("viewerCornerPlate"));
    QQuickItem *viewerReadout = itemNamed(controls, QStringLiteral("viewerZoomReadout"));
    QQuickItem *viewerReset = itemNamed(controls, QStringLiteral("viewerResetButton"));
    QQuickItem *viewerSwitch = itemNamed(controls, QStringLiteral("spectrogramPopupModeSwitch"));
    QVERIFY(cornerPlate && viewerReadout && viewerReset && viewerSwitch);
    QTRY_VERIFY(viewerReadout->isVisible() && viewerReset->isVisible());
    QCOMPARE(sceneRectOf(cornerPlate), sceneRectOf(controls));
    QCOMPARE(sceneRectOf(cornerPlate).height(), plate->height());
    for (const char *part : {"plotPlateFill", "plotPlateEdge"}) {
        QQuickItem *mine = itemNamed(plate, QString::fromLatin1(part));
        QQuickItem *theirs = itemNamed(cornerPlate, QString::fromLatin1(part));
        QVERIFY(mine != nullptr && theirs != nullptr && theirs->isVisible());
        QCOMPARE(theirs->property("color").value<QColor>(), mine->property("color").value<QColor>());
        QCOMPARE(theirs->property("radius").toDouble(), mine->property("radius").toDouble());
    }
    {
        QQuickItem *afterZoom = itemNamed(controls, QStringLiteral("viewerControlsDivider"));
        QQuickItem *beforeClose = itemNamed(controls, QStringLiteral("viewerControlsCloseDivider"));
        QVERIFY(afterZoom && beforeClose && afterZoom->isVisible() && beforeClose->isVisible());
        QCOMPARE(sceneRectOf(afterZoom).left(), sceneRectOf(viewerReset).right() + 4);
        QCOMPARE(sceneRectOf(viewerSwitch).left(), sceneRectOf(afterZoom).right() + 8);
        QCOMPARE(sceneRectOf(beforeClose).left(), sceneRectOf(viewerSwitch).right() + 8);
        for (QQuickItem *divider : {afterZoom, beforeClose}) {
            QCOMPARE(divider->width(), 1.0);
            QCOMPARE(sceneRectOf(divider).top() - sceneRectOf(cornerPlate).top(), 6.0);
            QCOMPARE(sceneRectOf(cornerPlate).bottom() - sceneRectOf(divider).bottom(), 6.0);
            QCOMPARE(divider->property("color").value<QColor>(),
                     itemNamed(cornerPlate, QStringLiteral("plotPlateEdge"))->property("border").value<QObject *>()
                         ->property("color").value<QColor>());
        }
    }
    // The pointer between the groups is on the plate: the viewer's
    // controls stay and the plot's crosshair hides.
    QTest::mouseMove(window, sceneRectOf(viewerSwitch).topRight().toPoint() + QPoint(2, -2));
    QTRY_VERIFY(layer->property("plateHovered").toBool());
    // Without a zoom, the plate starts at the mode switch, with one divider.
    spectrogramSurface->setProperty("_viewerZoomLevel", 1.0);
    QTRY_VERIFY(!viewerReadout->isVisible());
    QTRY_COMPARE(sceneRectOf(cornerPlate).left(), sceneRectOf(viewerSwitch).left() - 4);
    QVERIFY(visibleItemNamed(window, QStringLiteral("viewerControlsDivider")) == nullptr);
    spectrogramSurface->setProperty("_viewerZoomLevel", 2.0);
    QTRY_VERIFY(viewerReadout->isVisible());
    // The readout is plain text in the plot's secondary colour, and Reset a
    // glyph button, not in the accent at rest.
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QObject *plot = qvariant_cast<QObject *>(palette->property("spectrogramPlot"));
    QCOMPARE(viewerReadout->property("color").value<QColor>().rgba(),
             plot->property("plotTextSecondary").value<QColor>().rgba());
    QCOMPARE(viewerReset->property("display").toInt(), 0); // AbstractButton.IconOnly
    QVERIFY(!viewerReset->property("checked").toBool());
    // The readout 6 px in from the plate, and 6 px from Reset's glyph box.
    QQuickItem *resetGlyph = itemNamed(viewerReset, QStringLiteral("viewerToolButtonIcon"));
    QVERIFY(resetGlyph != nullptr);
    const double textLeft = viewerReadout->mapToScene(QPointF()).x()
        + viewerReadout->property("leftPadding").toDouble();
    const double textRight = textLeft + viewerReadout->property("contentWidth").toDouble();
    QCOMPARE(textLeft - sceneRectOf(cornerPlate).left(), 6.0);
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

    // The viewer's Reset, beside the mode switch in its title strip, does
    // the same zoom-only reset.
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QQuickItem *reset = itemNamed(strip, QStringLiteral("viewerResetButton"));
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
    // Shown on the screen: open, with its items in a window that shows.
    const auto contextMenuShown = [window]() {
        for (QObject *menu : window->findChildren<QObject *>(QStringLiteral("albumArtViewerContextMenu"))) {
            auto *content = qvariant_cast<QQuickItem *>(menu->property("contentItem"));
            if (menu->property("visible").toBool() && content != nullptr && content->window() != nullptr
                && content->window()->isExposed()) {
                return true;
            }
        }
        return false;
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
        // Over the whole screen, the mode switch is in the strip.
        QQuickItem *modeButton = visibleItemNamed(host, fullscreenMode == 1
            ? QStringLiteral("spectrogramStripModeButton1")
            : QStringLiteral("spectrogramPopupModeButton1"));
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

// Over the whole screen, with its title strip on, the visualization viewer's
// controls are all in the strip, as the album art viewer's are (Close in the
// strip and the mode switch on a plate 5 px under it read as two clusters,
// and Close's tooltip covered the plate): Spectrogram and Waveform, the
// current one in the strip's look for a choice that is on, then a 1 px
// divider with 4 px either side, then Close. The plot's corner has no plate.
// They are radio buttons to assistive technology, Tab takes them in order,
// and each one's tooltip is clear of the others.
void TransportInputSmokeTest::viewerButtonsRingOnlyForTheKeyboard_data() {
    QTest::addColumn<int>("fullscreenMode");
    QTest::newRow("window") << 0;
    QTest::newRow("whole screen") << 1;
}

// The focus ring is the keyboard's alone: a press on a viewer button takes
// the focus for the mouse, so the button the keyboard reached before lets
// it go with its ring (Close kept its ring while the pointer pressed
// another button, two buttons marked at once), and the pressed one shows
// none. The focus a click gave holds neither the controls, which hide
// after the pointer rests as before, nor Space, which plays and pauses.
void TransportInputSmokeTest::viewerButtonsRingOnlyForTheKeyboard() {
    QFETCH(int, fullscreenMode);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QVERIFY(bridge != nullptr && viewer != nullptr && autoHide != nullptr && playPause != nullptr);
    setPlayingTrack(bridge);
    autoHide->setProperty("hideDelay", 600000);
    bridge->setProperty("viewerFullscreenMode", fullscreenMode);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *host = window;
    QString modeName = QStringLiteral("spectrogramPopupModeButton");
    QString closeName = QStringLiteral("spectrogramPopupCloseButton");
    if (fullscreenMode == 1) {
        host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
        QVERIFY(host != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(host));
        modeName = QStringLiteral("spectrogramStripModeButton");
        closeName = QStringLiteral("spectrogramFullscreenCloseButton");
    } else {
        QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
        QVERIFY(popup != nullptr);
        QTRY_VERIFY(popup->property("opened").toBool());
    }
    QQuickItem *close = nullptr;
    QTRY_VERIFY((close = visibleItemNamed(host, closeName)) != nullptr);
    QQuickItem *waveformButton = visibleItemNamed(host, modeName + QLatin1Char('1'));
    QVERIFY(waveformButton != nullptr);
    QQuickItem *closeRing = itemNamed(close, QStringLiteral("viewerToolButtonFocusRing"));
    QQuickItem *waveformRing = itemNamed(waveformButton, QStringLiteral("viewerToolButtonFocusRing"));
    QVERIFY(closeRing != nullptr && waveformRing != nullptr);

    // The keyboard on Close: its ring.
    for (int press = 0; press < 12 && !close->hasActiveFocus(); ++press) {
        QTest::keyClick(host, Qt::Key_Tab);
    }
    QVERIFY(close->hasActiveFocus());
    QTRY_VERIFY(closeRing->isVisible());

    // The pointer pressing the waveform's button: no ring on either.
    const QPoint inside = centerInWindow(waveformButton);
    QTest::mouseMove(host, inside);
    QTest::mousePress(host, Qt::LeftButton, Qt::NoModifier, inside);
    QTRY_VERIFY(waveformButton->property("down").toBool());
    QVERIFY(waveformButton->hasActiveFocus());
    QCOMPARE(waveformButton->property("focusReason").toInt(), int(Qt::MouseFocusReason));
    QVERIFY(!closeRing->isVisible());
    QVERIFY(!waveformRing->isVisible());
    QTest::mouseRelease(host, Qt::LeftButton, Qt::NoModifier, inside);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QVERIFY(!waveformRing->isVisible());

    // Space plays and pauses, rather than pressing the clicked button.
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    viewer->setProperty("visualizationMode", 0);
    QTRY_VERIFY(!waveformButton->property("checked").toBool());
    QTest::keyClick(host, Qt::Key_Space);
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(viewer->property("visualizationMode").toInt(), 0);

    // Nor does it hold the controls: they hide after the pointer rests.
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());

    autoHide->setProperty("hideDelay", 600000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    if (fullscreenMode == 1) {
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

// In the album art viewer in the window, a click on Image Details, which
// takes the focus for the mouse, doesn't hold its controls: they hide after
// the pointer rests, as the visualization viewer's do (they stayed for as
// long as the button had the focus). The keyboard's focus on it still holds
// them.
void TransportInputSmokeTest::albumArtViewerClickedDetailsLetTheControlsHide() {
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
    QObject *autoHide = window->findChild<QObject *>(QStringLiteral("albumArtFullscreenControlsAutoHide"));
    auto *art = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge && viewer && autoHide && art);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", 0);
    autoHide->setProperty("hideDelay", 600000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QTRY_COMPARE(art->property("imageStatus").toInt(), 1);
    const QPoint onArt = sceneRectOf(art).center().toPoint();
    QTest::mouseMove(window, onArt + QPoint(1, 1));
    QTest::mouseMove(window, onArt);
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    QQuickItem *details = nullptr;
    QTRY_VERIFY((details = visibleItemNamed(window, QStringLiteral("albumArtPopupInfoButton"))) != nullptr);

    // A click on Image Details: its focus is the mouse's, and the controls
    // hide once the pointer rests.
    const QPoint inside = centerInWindow(details);
    QTest::mouseMove(window, inside);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, inside);
    QTRY_VERIFY(details->hasActiveFocus());
    QCOMPARE(details->property("focusReason").toInt(), int(Qt::MouseFocusReason));
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());

    // Back, and the keyboard on it: they stay while it has the focus.
    autoHide->setProperty("hideDelay", 600000);
    QTest::mouseMove(window, onArt + QPoint(1, 1));
    QTest::mouseMove(window, onArt);
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    for (int press = 0; press < 12 && !(details->hasActiveFocus()
             && details->property("focusReason").toInt() == int(Qt::TabFocusReason)); ++press) {
        QTest::keyClick(window, Qt::Key_Tab);
    }
    QVERIFY(details->hasActiveFocus());
    QCOMPARE(details->property("focusReason").toInt(), int(Qt::TabFocusReason));
    QTRY_VERIFY(autoHide->property("held").toBool());
    autoHide->setProperty("hideDelay", 1);
    QTest::qWait(100);
    QVERIFY(autoHide->property("controlsVisible").toBool());

    autoHide->setProperty("hideDelay", 600000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

void TransportInputSmokeTest::viewerCornerPlateClearsTheLevelMeter_data() {
    QTest::addColumn<int>("fullscreenMode");
    QTest::newRow("window") << 0;
    QTest::newRow("whole screen, no strip") << 1;
}

// The visualization viewer's corner plate, in the window and over the whole
// screen with the strip turned off, keeps 8 px from the level meter's
// column (at 5 px its edge all but met the meter's seam), and 5 px from the
// plot's corner without the meter; what the plot keeps clear of follows it.
// Over the whole screen it is on the strip's scale, the strip's buttons,
// glyphs and readout, 8 px in from the screen's top and right edges (it
// fell back to the window's chrome, 24 px buttons 5 px from the corner,
// beside lane heads on the larger type).
void TransportInputSmokeTest::viewerCornerPlateClearsTheLevelMeter() {
    QFETCH(int, fullscreenMode);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *meter = window->findChild<QQuickItem *>(QStringLiteral("levelMeter"));
    QVERIFY(bridge && viewer && autoHide && surface && meter);
    setPlayingTrack(bridge);
    autoHide->setProperty("hideDelay", 600000);
    bridge->setProperty("showFullscreenTitleStrip", false);
    bridge->setProperty("viewerFullscreenMode", fullscreenMode);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *host = window;
    if (fullscreenMode == 1) {
        host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
        QVERIFY(host != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(host));
    } else {
        QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
        QVERIFY(popup != nullptr);
        QTRY_VERIFY(popup->property("opened").toBool());
    }
    QTRY_COMPARE(surface->window(), host);
    QTRY_COMPARE(sceneRectOf(surface).top(), 0.0);
    // The waveform there draws its lanes' seam as a viewer's.
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(waveformSurface != nullptr);
    QVERIFY(waveformSurface->property("viewerMode").toBool());
    QQuickItem *plate = nullptr;
    QTRY_VERIFY((plate = visibleItemNamed(host, fullscreenMode == 0 ? QStringLiteral("spectrogramPopupControls")
                                                                    : QStringLiteral("spectrogramFullscreenCorner")))
                != nullptr);
    const bool wholeScreen = fullscreenMode == 1;
    QQuickItem *buttons = itemNamed(plate, QStringLiteral("viewerCornerButtons"));
    QQuickItem *close = visibleItemNamed(host, wholeScreen ? QStringLiteral("spectrogramFullscreenCornerCloseButton")
                                                           : QStringLiteral("spectrogramPopupCloseButton"));
    QVERIFY(buttons != nullptr && close != nullptr);
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(palette != nullptr);
    if (wholeScreen) {
        QQuickItem *strip = itemNamed(host->contentItem(), QStringLiteral("spectrogramFullscreenStrip"));
        QVERIFY(strip != nullptr);
        QCOMPARE(close->height(), strip->property("buttonSize").toReal());
        QCOMPARE(close->property("glyphSize").toInt(), strip->property("iconSize").toInt());
        QCOMPARE(buttons->property("readoutPointSize").toReal(), strip->property("textPointSize").toReal());
        // As tall as the strip's band.
        QCOMPARE(plate->height(), strip->property("bandHeight").toReal());
    } else {
        QCOMPARE(close->height(), palette->property("toolButtonSize").toReal());
        QCOMPARE(close->property("glyphSize").toInt(), palette->property("toolIconSize").toInt());
        QCOMPARE(buttons->property("readoutPointSize").toReal(), palette->property("smallPointSize").toReal());
    }
    for (const bool meterShown : {true, false}) {
        bridge->setProperty("showLevelMeter", meterShown);
        QTRY_COMPARE(meter->isVisible(), meterShown);
        const qreal edge = meterShown ? sceneRectOf(meter).left() : sceneRectOf(surface).right();
        const qreal gap = meterShown || wholeScreen ? 8.0 : 5.0;
        QTRY_COMPARE(edge - sceneRectOf(plate).right(), gap);
        QCOMPARE(sceneRectOf(plate).top() - sceneRectOf(surface).top(), wholeScreen ? 8.0 : 5.0);
        // What the plot keeps clear of: the plate, its 1 px edge, the gap.
        const QSizeF reserved = surface->property("reservedCorner").toSizeF();
        QCOMPARE(reserved.width(), gap + plate->width() + 1.0);
        QCOMPARE(reserved.height(), sceneRectOf(plate).bottom() - sceneRectOf(surface).top() + 1.0);
    }

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(!waveformSurface->property("viewerMode").toBool());
    if (fullscreenMode == 1) {
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

void TransportInputSmokeTest::fullscreenVisualizationStripHoldsTheModeSwitch() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(bridge != nullptr && viewer != nullptr && autoHide != nullptr && layer != nullptr);
    setPlayingTrack(bridge);
    autoHide->setProperty("hideDelay", 600000);
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QTRY_COMPARE(layer->window(), fullscreen);
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QQuickItem *spectrogramButton = itemNamed(strip, QStringLiteral("spectrogramStripModeButton0"));
    QQuickItem *waveformButton = itemNamed(strip, QStringLiteral("spectrogramStripModeButton1"));
    QQuickItem *close = itemNamed(strip, QStringLiteral("spectrogramFullscreenCloseButton"));
    QQuickItem *divider = itemNamed(strip, QStringLiteral("viewerControlsCloseDivider"));
    QVERIFY(spectrogramButton != nullptr && waveformButton != nullptr && close != nullptr && divider != nullptr);

    // Nothing on the plot's corner.
    QTRY_VERIFY(!itemNamed(layer, QStringLiteral("visualizationModeSwitch"))->isVisible());
    QVERIFY(visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenCorner")) == nullptr);
    QVERIFY(!itemNamed(layer, QStringLiteral("plotZoomPlate"))->isVisible());

    // [Spectrogram Waveform] | [Close], 8 px in from the screen's edge, on
    // one line, the divider in the strip's edge colour, 8 px from the
    // switch's chosen segment and 4 px from Close.
    QTRY_COMPARE(sceneRectOf(close).right(), fullscreen->width() - 8.0);
    QTRY_VERIFY(spectrogramButton->isVisible() && waveformButton->isVisible() && divider->isVisible());
    QCOMPARE(sceneRectOf(waveformButton).left() - sceneRectOf(spectrogramButton).right(), 4.0);
    QCOMPARE(sceneRectOf(divider).left() - sceneRectOf(waveformButton).right(), 8.0);
    QCOMPARE(sceneRectOf(divider).width(), 1.0);
    QCOMPARE(sceneRectOf(close).left() - sceneRectOf(divider).right(), 4.0);
    for (QQuickItem *item : {spectrogramButton, waveformButton, divider}) {
        QCOMPARE(sceneRectOf(item).center().y(), sceneRectOf(close).center().y());
        QVERIFY(sceneRectOf(strip).contains(sceneRectOf(item)));
    }
    QCOMPARE(sceneRectOf(spectrogramButton).size(), sceneRectOf(close).size());
    QObject *plotColors = qvariant_cast<QObject *>(strip->property("plotColors"));
    QVERIFY(plotColors != nullptr);
    QCOMPARE(colorOf(divider, "color"), colorOf(plotColors, "plotPlateEdge"));

    // Each one's tooltip, under the pointer, covers none of the others.
    // Close's names its key, as Image Details' does.
    const QList<std::pair<QQuickItem *, QString>> tips{
        {spectrogramButton, QStringLiteral("Spectrogram")},
        {waveformButton, QStringLiteral("Waveform")},
        {close, QStringLiteral("Close (Esc)")},
    };
    // Where they show is the point, not after how long: at once.
    for (const auto &[button, text] : tips) {
        // In the button's own file, whose imports have ToolTip: the file
        // that declares a button need not.
        QQuickItem *inButton = itemNamed(button, QStringLiteral("viewerToolButtonIcon"));
        QVERIFY(inButton != nullptr);
        QVERIFY(evaluateOn(inButton, QStringLiteral("root.ToolTip.delay = 0")).isValid());
    }
    for (const auto &[button, text] : tips) {
        QTest::mouseMove(fullscreen, centerInWindow(button));
        QList<QQuickItem *> shown;
        QTRY_VERIFY((shown = visibleTextItems(fullscreen->contentItem(), text)).size() == 1);
        QQuickItem *popupItem = shown.constFirst()->parentItem();
        QVERIFY(popupItem != nullptr);
        const QRectF tip = sceneRectOf(popupItem);
        for (const auto &[otherButton, otherText] : tips) {
            QVERIFY2(!tip.intersects(sceneRectOf(otherButton)), qPrintable(text + QStringLiteral(" over ") + otherText));
        }
        QTest::mouseMove(fullscreen, QPoint(fullscreen->width() / 2, fullscreen->height() / 2));
        QTRY_VERIFY(visibleTextItems(fullscreen->contentItem(), text).isEmpty());
    }

    QAccessible::setActive(true);
    const QScopeGuard stopAccessibility([]() { QAccessible::setActive(false); });
    // Close's accessible name is the action alone, without the key.
    {
        QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(close);
        QVERIFY(accessible != nullptr);
        QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Close"));
    }
    // The current mode checked, in the strip's look, and a radio button.
    for (const int mode : {1, 0}) {
        viewer->setProperty("visualizationMode", mode);
        QQuickItem *chosen = mode == 0 ? spectrogramButton : waveformButton;
        QQuickItem *other = mode == 0 ? waveformButton : spectrogramButton;
        QTRY_VERIFY(chosen->property("checked").toBool());
        QVERIFY(!other->property("checked").toBool());
        QVERIFY(itemNamed(chosen, QStringLiteral("viewerToolButtonChecked"))->isVisible());
        QVERIFY(itemNamed(chosen, QStringLiteral("viewerToolButtonCheckedEdge"))->isVisible());
        QVERIFY(!itemNamed(other, QStringLiteral("viewerToolButtonChecked"))->isVisible());
        for (QQuickItem *button : {chosen, other}) {
            QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(button);
            QVERIFY(accessible != nullptr);
            QCOMPARE(accessible->role(), QAccessible::RadioButton);
            QVERIFY(accessible->state().checkable);
            QCOMPARE(bool(accessible->state().checked), button == chosen);
            QVERIFY(accessible->state().focusable);
        }
    }
    QCOMPARE(QAccessible::queryAccessibleInterface(spectrogramButton)->text(QAccessible::Name),
             QStringLiteral("Spectrogram"));
    QCOMPARE(QAccessible::queryAccessibleInterface(waveformButton)->text(QAccessible::Name),
             QStringLiteral("Waveform"));
    QCOMPARE(QAccessible::queryAccessibleInterface(itemNamed(strip, QStringLiteral("spectrogramStripModeSwitch")))
                 ->text(QAccessible::Name),
             QStringLiteral("Visualization"));

    // A click chooses; choosing the chosen one again leaves it chosen.
    for (int click = 0; click < 2; ++click) {
        QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(waveformButton));
        QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
        QTRY_VERIFY(waveformButton->property("checked").toBool());
        QVERIFY(!spectrogramButton->property("checked").toBool());
    }
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(spectrogramButton));
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QTRY_VERIFY(spectrogramButton->property("checked").toBool());

    // Tab: Spectrogram, Waveform, Close.
    spectrogramButton->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(fullscreen, Qt::Key_Tab);
    QTRY_COMPARE(fullscreen->activeFocusItem(), waveformButton);
    QTest::keyClick(fullscreen, Qt::Key_Tab);
    QTRY_COMPARE(fullscreen->activeFocusItem(), close);
    QTest::keyClick(fullscreen, Qt::Key_Tab, Qt::ShiftModifier);
    QTRY_COMPARE(fullscreen->activeFocusItem(), waveformButton);
    // Space chooses the focused one.
    QTest::keyClick(fullscreen, Qt::Key_Space);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QQuickItem *viewFocus = fullscreen->findChild<QQuickItem *>(QStringLiteral("spectrogramFullscreenViewFocus"));
    QVERIFY(viewFocus != nullptr);
    viewFocus->forceActiveFocus(Qt::MouseFocusReason);

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
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
    // strip, and the mode switch sits on a plate at the top edge, which
    // ends in Close behind its divider, as the windowed plate and the strip
    // do (it had no Close, and a pointer no way out in sight).
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
                                                    QStringLiteral("spectrogramFullscreenCornerModeSwitch")))
                != nullptr);
    QQuickItem *corner = itemNamed(fullscreen->contentItem(), QStringLiteral("spectrogramFullscreenCorner"));
    QVERIFY(corner != nullptr);
    QTRY_COMPARE(corner->opacity(), 1.0);
    QQuickItem *cornerClose = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenCornerCloseButton"));
    QQuickItem *cornerDivider = itemNamed(corner, QStringLiteral("viewerControlsCloseDivider"));
    QVERIFY(cornerClose != nullptr && cornerDivider != nullptr && cornerDivider->isVisible());
    QCOMPARE(sceneRectOf(cornerDivider).left(), sceneRectOf(modeSwitch).right() + 8.0);
    QCOMPARE(sceneRectOf(cornerClose).left(), sceneRectOf(cornerDivider).right() + 4.0);
    QCOMPARE(sceneRectOf(cornerClose).center().y(), sceneRectOf(modeSwitch).center().y());
    QVERIFY(sceneRectOf(corner).contains(sceneRectOf(cornerClose)));
    QVERIFY(sceneRectOf(modeSwitch).top() >= 0.0);
    QVERIFY2(sceneRectOf(modeSwitch).top() < strip->height(), qPrintable(QString::number(sceneRectOf(modeSwitch).top())));
    // They still hide as the pointer rests.
    autoHide->setProperty("hideDelay", 50);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTRY_VERIFY(!modeSwitch->isVisible());
    QVERIFY(!strip->isVisible());

    // Turned on again, it shows with the controls, which keep below it,
    // and it has the mode switch, which is no longer on the plot.
    QVERIFY(bridge->setProperty("showFullscreenTitleStrip", true));
    QTRY_COMPARE(plotLayer->property("topInset").toReal(), strip->height());
    autoHide->setProperty("hideDelay", 600000);
    QTest::mouseMove(fullscreen, QPoint(300, 300));
    QTRY_VERIFY(strip->isVisible());
    QQuickItem *stripModeSwitch = itemNamed(strip, QStringLiteral("spectrogramStripModeSwitch"));
    QVERIFY(stripModeSwitch != nullptr);
    QTRY_VERIFY(stripModeSwitch->isVisible());
    QVERIFY(!modeSwitch->isVisible());
    // Turned off once more, the switch is on its plate again, and its
    // Close closes the viewer.
    QVERIFY(bridge->setProperty("showFullscreenTitleStrip", false));
    QTRY_VERIFY(modeSwitch->isVisible());
    QVERIFY(!strip->isVisible());
    QTRY_VERIFY(cornerClose->isVisible() && corner->opacity() == 1.0);
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(cornerClose));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
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
    QTRY_COMPARE(albumArt->property("infoTopInset").toReal(), albumArtStrip->height());
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

namespace {

// Detaches `waveform` from the playhead at a zoom of 4, with a middle-drag
// across it that moves the view on by its width.
bool detachWaveform(WaveformEditorItem *waveform) {
    if (waveform->zoomLevel() < 4.0) waveform->setZoomLevel(4.0);
    const double width = std::floor(waveform->width());
    const auto send = [waveform](QEvent::Type type, double x, Qt::MouseButton button,
                                 Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(x, 20.0), QPointF(x, 20.0), button, buttons, Qt::NoModifier);
        if (type == QEvent::MouseButtonPress) {
            waveform->mousePressEvent(&event);
        } else if (type == QEvent::MouseMove) {
            waveform->mouseMoveEvent(&event);
        } else {
            waveform->mouseReleaseEvent(&event);
        }
    };
    send(QEvent::MouseButtonPress, width, Qt::MiddleButton, Qt::MiddleButton);
    send(QEvent::MouseMove, width / 2, Qt::NoButton, Qt::MiddleButton);
    send(QEvent::MouseMove, 0.0, Qt::NoButton, Qt::MiddleButton);
    send(QEvent::MouseButtonRelease, 0.0, Qt::MiddleButton, Qt::NoButton);
    return waveform->detached();
}

// The view of the main spectrogram's lanes, which they share; null if
// they do not.
SpectrogramViewFollow *spectrogramFollow(QQuickWindow *window) {
    SpectrogramViewFollow *shared = nullptr;
    const QList<QQuickItem *> panes =
        itemsNamed(window->contentItem(), QStringLiteral("spectrogramPaneItem"));
    for (QQuickItem *pane : panes) {
        auto *spectrogram = qobject_cast<SpectrogramItem *>(pane);
        if (spectrogram == nullptr || (shared != nullptr && spectrogram->viewFollow() != shared)) {
            return nullptr;
        }
        shared = spectrogram->viewFollow();
    }
    return shared;
}

} // namespace

namespace {

// An item's box in its window: a Text's boundingRect is its text's alone,
// without the padding.
QRectF boxOf(const QQuickItem *item) {
    return item->mapRectToScene(QRectF(QPointF(), item->size()));
}

// A middle click on `plot` at `x`, which starts autoscroll.
void middleClickPlot(QQuickItem *plot, double x) {
    const QPointF at(x, 20.0);
    QMouseEvent press(QEvent::MouseButtonPress, at, plot->mapToScene(at), plot->mapToScene(at),
                      Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(plot, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, plot->mapToScene(at), plot->mapToScene(at),
                        Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(plot, &release);
}

} // namespace

void TransportInputSmokeTest::movedViewSaysSoInTheCorner_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("spectrogram") << 0;
    QTest::newRow("waveform") << 1;
}

// A view the user moved looked like one that follows. The zoom's plate
// says "Not following", and the corner's row stays at rest for as long.
// The plate grows to the left; Reset and the switch keep their places.
// Never while autoscroll runs, whose cursor alone says so (#655).
void TransportInputSmokeTest::movedViewSaysSoInTheCorner() {
    QFETCH(int, mode);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *followAction = window->findChild<QObject *>(QStringLiteral("followPlayheadAction"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("visualizationSurface"));
    QVERIFY(bridge && viewer && followAction && spectrogramSurface && waveformSurface && layer && surface);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("spectrogramDisplayMode", 1);
    viewer->setProperty("visualizationMode", mode);
    QTRY_VERIFY(!surface->property("modeSwitching").toBool());

    QQuickItem *plot = waveform;
    if (mode == 0) {
        auto *pane = qobject_cast<SpectrogramItem *>(
            itemNamed(spectrogramSurface, QStringLiteral("spectrogramPaneItem")));
        QVERIFY(pane != nullptr);
        constexpr int bins = 16;
        constexpr int columns = 10'000;
        pane->feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns, 0, columns,
                                   48'000, 1'024, true, true, 1, false);
        QTRY_VERIFY(pane->precomputedReady());
        plot = pane;
    }
    const auto zoom = [&](double level) {
        if (mode == 0) {
            spectrogramSurface->setProperty("_widgetZoomLevel", level);
        } else {
            waveform->setZoomLevel(level);
        }
    };
    const auto moveView = [&]() {
        if (mode == 1) {
            return detachWaveform(waveform);
        }
        SpectrogramViewFollow *follow = spectrogramFollow(window);
        if (follow == nullptr) {
            return false;
        }
        follow->detach(30.0, 0.5);
        return follow->detached();
    };
    const auto moved = [surface]() { return surface->property("viewMoved").toBool(); };

    QQuickItem *plate = itemNamed(layer, QStringLiteral("plotZoomPlate"));
    QQuickItem *readout = itemNamed(layer, QStringLiteral("plotZoomReadout"));
    QQuickItem *reset = itemNamed(layer, QStringLiteral("plotZoomResetButton"));
    QQuickItem *notice = itemNamed(layer, QStringLiteral("plotFollowNotice"));
    QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(plate && readout && reset && notice && modeSwitch);
    const QPoint away(8, window->height() - 8);
    QTest::mouseMove(window, away);
    zoom(4.0);

    // Following: no row at rest, and nothing that says so.
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
    QTRY_COMPARE(plate->opacity(), 0.0);
    QVERIFY(!notice->isVisible() && !moved());
    window->grabWindow();
    const QRectF resetAt = sceneRectOf(reset);
    const QRectF switchAt = sceneRectOf(modeSwitch);
    const QRectF plateAt = sceneRectOf(plate);

    // Moved, the pointer still away: the whole row, with the notice in the
    // readout's ink, grown to the left.
    QVERIFY(moveView());
    QTRY_VERIFY(moved() && notice->isVisible());
    QTRY_COMPARE(plate->opacity(), 1.0);
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    window->grabWindow();
    QQuickItem *text = itemNamed(notice, QStringLiteral("followNoticeText"));
    QVERIFY(text != nullptr);
    QCOMPARE(text->property("text").toString(), QStringLiteral("Not following"));
    QCOMPARE(text->property("color").value<QColor>(), readout->property("color").value<QColor>());
    QTRY_VERIFY(sceneRectOf(plate).left() < plateAt.left());
    QCOMPARE(sceneRectOf(plate).right(), plateAt.right());
    QCOMPARE(sceneRectOf(reset), resetAt);
    QCOMPARE(sceneRectOf(modeSwitch), switchAt);
    QVERIFY(sceneRectOf(notice).right() <= sceneRectOf(readout).left());
    QVERIFY(sceneRectOf(plate).contains(sceneRectOf(notice)));
    QQuickItem *divider = itemNamed(notice, QStringLiteral("followNoticeDivider"));
    QVERIFY(divider != nullptr && divider->isVisible());
    QCOMPARE(sceneRectOf(divider).left() - boxOf(text).right(), 6.0);
    QVERIFY(surface->property("reservedCorner").toSizeF().width() > plate->width() + modeSwitch->width());
    // The zoom's plate and the mode switch's 6 px apart, edge to edge, each
    // edge 1 px outside its plate (with 4 px between the plates, their
    // edges were 2 px apart, pinched together), and the plot keeps clear of
    // the zoom's edge.
    QCOMPARE((sceneRectOf(modeSwitch).left() - 1.0) - (sceneRectOf(plate).right() + 1.0), 6.0);
    QCOMPARE(surface->property("reservedCorner").toSizeF().width(),
             sceneRectOf(layer).right() - (sceneRectOf(plate).left() - 1.0));

    // Its tooltip names the way back and that action's shortcut as it is.
    QObject *toolTip = notice->findChild<QObject *>(QStringLiteral("followNoticeToolTip"));
    QVERIFY(toolTip != nullptr);
    QCOMPARE(toolTip->property("message").toString(),
             QStringLiteral("Follow Playhead (F) brings the view back to the playhead"));
    followAction->setProperty("defaultKeyBindings", QStringList{QStringLiteral("J")});
    QTRY_COMPARE(toolTip->property("message").toString(),
                 QStringLiteral("Follow Playhead (J) brings the view back to the playhead"));
    followAction->setProperty("defaultKeyBindings", QStringList());
    QTRY_COMPARE(toolTip->property("message").toString(),
                 QStringLiteral("Follow Playhead in the View menu brings the view back to the playhead"));
    followAction->setProperty("defaultKeyBindings", QStringList{QStringLiteral("F")});
    QTest::mouseMove(window, centerInWindow(text) + QPoint(1, 0));
    QTest::mouseMove(window, centerInWindow(text));
    QTRY_VERIFY(toolTip->property("visible").toBool());
    QTest::mouseMove(window, away);

    // Followed again with the pointer away, the notice goes with the row's
    // fade, its text with it (notFollowingLeavesOneWay).
    QVERIFY(QMetaObject::invokeMethod(surface, "followPlayhead"));
    QVERIFY(!moved());
    QVERIFY(text->isVisible());
    QTRY_COMPARE(plate->opacity(), 0.0);
    QTRY_VERIFY(!notice->isVisible());
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);

    // With the pointer on the pane, its text fades out as the plate
    // narrows (notFollowingMovesAsOnePlate).
    QVERIFY(moveView());
    QTest::mouseMove(window, layer->mapToScene(QPointF(layer->width() / 2, layer->height() / 2)).toPoint());
    QTRY_VERIFY(notice->isVisible() && text->isVisible());
    QVERIFY(QMetaObject::invokeMethod(surface, "followPlayhead"));
    QTRY_VERIFY(!notice->isVisible());

    // Autoscroll from a following view: never moved while it runs, not for
    // one binding's evaluation, nor as Follow Playhead ends it.
    QSignalSpy movedChanges(surface, SIGNAL(viewMovedChanged()));
    middleClickPlot(plot, plot->width() / 2);
    QVERIFY(surface->property("autoscrolling").toBool());
    QVERIFY(surface->property("detached").toBool());
    QVERIFY(!moved() && !notice->isVisible());
    QVERIFY(QMetaObject::invokeMethod(surface, "followPlayhead"));
    QTRY_VERIFY(!surface->property("autoscrolling").toBool());
    QCOMPARE(movedChanges.count(), 0);
    // The lane that autoscrolled stops as it is told, queued.
    QTRY_VERIFY(QGuiApplication::overrideCursor() == nullptr);
    // Stopped on the moved view, it may say so; from a moved view it goes.
    middleClickPlot(plot, plot->width() / 2);
    QVERIFY(surface->property("autoscrolling").toBool());
    QVERIFY(!notice->isVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!surface->property("autoscrolling").toBool());
    QTRY_VERIFY(moved() && notice->isVisible());
    QVERIFY(text->isVisible());
    middleClickPlot(plot, plot->width() / 2);
    QVERIFY(surface->property("autoscrolling").toBool());
    QVERIFY(!moved() && !notice->isVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(notice->isVisible());

    if (mode == 0) {
        // At the default zoom: the notice alone, the switch where it was.
        zoom(1.0);
        QVERIFY(moveView());
        QTest::mouseMove(window, away);
        QTRY_VERIFY(notice->isVisible() && plate->isVisible());
        QVERIFY(!readout->isVisible() && !reset->isVisible());
        QVERIFY(!divider->isVisible());
        QTRY_COMPARE(plate->opacity(), 1.0);
        QCOMPARE(sceneRectOf(modeSwitch), switchAt);
        window->grabWindow();
        QCOMPARE(sceneRectOf(plate).right() + 8, sceneRectOf(modeSwitch).left());
        // 6 px of plate either side of the text.
        QCOMPARE(boxOf(text).left() + text->property("leftPadding").toDouble() - sceneRectOf(plate).left(), 6.0);
        QCOMPARE(sceneRectOf(plate).right() - boxOf(text).right(), 6.0);
    }

    // Reset Zoom follows again.
    QVERIFY(QMetaObject::invokeMethod(surface, "resetZoom"));
    QTRY_VERIFY(!moved() && !notice->isVisible());
}

namespace {

// The main pane's corner, a track playing in `mode`: 0 the centered
// spectrogram, with columns to show, or 1 the waveform.
struct PlotCorner {
    QQuickWindow *window = nullptr;
    QQuickItem *surface = nullptr;
    QQuickItem *layer = nullptr;
    QQuickItem *spectrogramSurface = nullptr;
    WaveformEditorItem *waveform = nullptr;
    QQuickItem *plate = nullptr;
    QQuickItem *readout = nullptr;
    QQuickItem *reset = nullptr;
    QQuickItem *notice = nullptr;
    QQuickItem *text = nullptr;
    QQuickItem *divider = nullptr;
    QQuickItem *modeSwitch = nullptr;
    int mode = 0;

    void zoom(double level) const {
        if (mode == 0) {
            spectrogramSurface->setProperty("_widgetZoomLevel", level);
        } else {
            waveform->setZoomLevel(level);
        }
    }
    bool moveView() const {
        if (mode == 1) {
            return detachWaveform(waveform);
        }
        SpectrogramViewFollow *follow = spectrogramFollow(window);
        if (follow == nullptr) {
            return false;
        }
        follow->detach(30.0, 0.5);
        return follow->detached();
    }
    bool moved() const { return surface->property("viewMoved").toBool(); }
    QPoint away() const { return {8, window->height() - 8}; }
    QPoint onPane() const {
        return layer->mapToScene(QPointF(layer->width() / 2, layer->height() / 2)).toPoint();
    }
};

// Loads the main window with its corner in `mode`; an error, or "".
QString loadPlotCorner(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel, int mode,
                       PlotCorner &corner) {
    corner.mode = mode;
    corner.window = loadActiveMainWindow(engine, libraryModel);
    if (corner.window == nullptr) return QStringLiteral("Main.qml failed to show");
    QQuickWindow *window = corner.window;
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    corner.spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    corner.layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    corner.surface = window->findChild<QQuickItem *>(QStringLiteral("visualizationSurface"));
    if (!bridge || !viewer || !corner.spectrogramSurface || !waveformSurface || !corner.layer || !corner.surface) {
        return QStringLiteral("the main window's plot is missing");
    }
    corner.waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    if (corner.waveform == nullptr) return QStringLiteral("no waveform");
    setPlayingTrack(bridge);
    bridge->setProperty("spectrogramDisplayMode", 1);
    viewer->setProperty("visualizationMode", mode);
    if (!QTest::qWaitFor([&]() { return !corner.surface->property("modeSwitching").toBool(); })) {
        return QStringLiteral("the mode switch did not end");
    }
    if (mode == 0) {
        auto *pane = qobject_cast<SpectrogramItem *>(
            itemNamed(corner.spectrogramSurface, QStringLiteral("spectrogramPaneItem")));
        if (pane == nullptr) return QStringLiteral("no spectrogram lane");
        constexpr int bins = 16;
        constexpr int columns = 10'000;
        pane->feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns, 0, columns,
                                   48'000, 1'024, true, true, 1, false);
        if (!QTest::qWaitFor([pane]() { return pane->precomputedReady(); })) {
            return QStringLiteral("the spectrogram has no columns");
        }
    }
    corner.plate = itemNamed(corner.layer, QStringLiteral("plotZoomPlate"));
    corner.readout = itemNamed(corner.layer, QStringLiteral("plotZoomReadout"));
    corner.reset = itemNamed(corner.layer, QStringLiteral("plotZoomResetButton"));
    corner.notice = itemNamed(corner.layer, QStringLiteral("plotFollowNotice"));
    corner.modeSwitch = itemNamed(corner.layer, QStringLiteral("visualizationModeSwitch"));
    if (!corner.plate || !corner.readout || !corner.reset || !corner.notice || !corner.modeSwitch) {
        return QStringLiteral("the corner's items are missing");
    }
    corner.text = itemNamed(corner.notice, QStringLiteral("followNoticeText"));
    corner.divider = itemNamed(corner.notice, QStringLiteral("followNoticeDivider"));
    if (!corner.text || !corner.divider) return QStringLiteral("the notice's items are missing");
    return {};
}

} // namespace

void TransportInputSmokeTest::notFollowingLeavesOneWay_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("cornerWanted");
    QTest::addColumn<bool>("zoomed");
    for (const int mode : {0, 1}) {
        for (const bool wanted : {true, false}) {
            for (const bool zoomed : {true, false}) {
                // The waveform is moved only while zoomed.
                if (mode == 1 && !zoomed) continue;
                QTest::addRow("%s, corner %s, %s", mode == 0 ? "spectrogram" : "waveform",
                              wanted ? "wanted" : "not wanted", zoomed ? "zoomed" : "at the default zoom")
                    << mode << wanted << zoomed;
            }
        }
    }
}

// As the view follows again, "Not following" leaves the same way in both
// plots. With the corner's row staying, zoomed, its text fades out as the
// plate narrows from its width to its new one at its left edge over 90 ms,
// Reset and the switch staying (its edge jumped 89 px in a frame, H15;
// notFollowingMovesAsOnePlate has it frame by frame). At the default zoom,
// with nothing else on it, the plate fades at its width with its text over
// 120 ms, OutCubic: the text went in a frame while the empty plate faded
// on (H16). With the row fading out, the notice goes
// with the plate's fade, at its width, on the row's curve: the plate
// narrowed 15 ms ahead of the row's fade, its text gone at full opacity
// (H17).
void TransportInputSmokeTest::notFollowingLeavesOneWay() {
    QFETCH(int, mode);
    QFETCH(bool, cornerWanted);
    QFETCH(bool, zoomed);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    QTest::mouseMove(window, cornerWanted ? corner.onPane() : corner.away());
    corner.zoom(zoomed ? 4.0 : 1.0);
    if (zoomed) {
        QTRY_VERIFY(corner.readout->isVisible());
    }
    window->grabWindow();
    const QRectF resetAt = sceneRectOf(corner.reset);
    const QRectF switchAt = sceneRectOf(corner.modeSwitch);
    // Where the plate's edge goes back to.
    const double plainLeft = sceneRectOf(corner.plate).left();

    QVERIFY(corner.moveView());
    QTRY_VERIFY(corner.moved() && corner.text->isVisible());
    const QPoint pointer = cornerWanted ? corner.onPane() : corner.away();
    QTest::mouseMove(window, pointer + QPoint(1, 1));
    QTest::mouseMove(window, pointer);
    QTRY_COMPARE(corner.layer->property("cornerWanted").toBool(), cornerWanted);
    QTRY_COMPARE(corner.plate->opacity(), 1.0);
    QTRY_COMPARE(corner.text->opacity(), 1.0);
    window->grabWindow();
    const double grownLeft = sceneRectOf(corner.plate).left();
    if (zoomed) {
        QVERIFY(grownLeft < plainLeft - 40);
    }

    const qreal ratio = window->effectiveDevicePixelRatio();
    const auto pixel = [ratio](const QImage &image, double x, double y) {
        return image.pixelColor(static_cast<int>(std::floor(x * ratio)), static_cast<int>(std::floor(y * ratio)));
    };
    // Zoomed: the plate's edge along its top, which shows on either
    // theme's plot (the light waveform's fill is its floor), left of the
    // readout's text, where the plate stays, in the middle of the room it
    // gives up, and the plot beside it.
    struct Fills {
        QColor staying;
        QColor room;
        QColor plot;
        double roomWidth = 0.0;
    };
    const auto fills = [&](const QImage &image) {
        const QRectF plate = sceneRectOf(corner.plate);
        const double text = boxOf(corner.readout).left() + corner.readout->property("leftPadding").toDouble();
        const double y = plate.top() - 0.5;
        Fills f;
        f.roomWidth = text - 7 - plate.left();
        f.staying = pixel(image, text - 3, y);
        f.room = pixel(image, plate.left() + f.roomWidth / 2, y);
        f.plot = pixel(image, plate.left() - 12, y);
        return f;
    };

    SteppedAnimationDriver animations;
    QVERIFY(QMetaObject::invokeMethod(corner.surface, "followPlayhead"));
    QVERIFY(!corner.moved());
    // The frame it follows in: the plate as it was, its text on it.
    const bool narrows = zoomed && cornerWanted;
    QVERIFY(corner.text->isVisible());
    QCOMPARE(corner.divider->isVisible(), zoomed);
    QImage image = window->grabWindow();
    QCOMPARE(sceneRectOf(corner.plate).left(), grownLeft);
    QCOMPARE(corner.plate->opacity(), 1.0);
    if (narrows) {
        // One plate, its room and the rest.
        const Fills f = fills(image);
        QVERIFY2(channelDistance(f.room, f.staying) <= 1, qPrintable(f.room.name() + u' ' + f.staying.name()));
    } else {
        QCOMPARE(corner.text->opacity(), 1.0);
    }
    double left = grownLeft;
    double opacity = 1.0;
    double textLevel = 1.0;
    // Over 90 ms as it narrows, 120 ms as it fades, or the row's 100 ms as
    // it fades out.
    for (int frame = 1; frame * 16 < (narrows ? 90 : cornerWanted ? 120 : 100); ++frame) {
        QVERIFY(animations.step(16));
        image = window->grabWindow();
        const double now = sceneRectOf(corner.plate).left();
        const QString where = QStringLiteral("frame %1").arg(frame);
        if (narrows) {
            // Narrowing, its right edge where it was, its text fading out.
            QVERIFY2((frame == 1 ? now > left && now < plainLeft : now >= left) && now <= plainLeft,
                     qPrintable(QStringLiteral("frame %1: %2 after %3, to %4").arg(frame).arg(now).arg(left).arg(plainLeft)));
            left = now;
            const double text = corner.notice->isVisible() ? corner.text->opacity() : 0.0;
            QVERIFY2(text < textLevel || text == 0.0, qPrintable(QStringLiteral("%1: %2 after %3").arg(where).arg(text).arg(textLevel)));
            textLevel = text;
            QCOMPARE(sceneRectOf(corner.reset), resetAt);
        } else {
            // At its width, fading with its text, and with the row as it
            // fades out.
            QCOMPARE(now, grownLeft);
            QVERIFY2(corner.text->isVisible() && corner.text->opacity() == 1.0, qPrintable(where));
            QCOMPARE(corner.divider->isVisible(), zoomed);
            QVERIFY2(corner.plate->opacity() < opacity,
                     qPrintable(QStringLiteral("%1: %2 after %3").arg(where).arg(corner.plate->opacity()).arg(opacity)));
            opacity = corner.plate->opacity();
            if (!cornerWanted) {
                QVERIFY2(corner.plate->opacity() == corner.modeSwitch->opacity(),
                         qPrintable(QStringLiteral("%1: plate %2, switch %3")
                                        .arg(where).arg(corner.plate->opacity()).arg(corner.modeSwitch->opacity())));
            }
        }
        QCOMPARE(sceneRectOf(corner.plate).right() + 8, switchAt.left());
        QCOMPARE(sceneRectOf(corner.modeSwitch), switchAt);
        if (cornerWanted && zoomed) {
            QCOMPARE(corner.plate->opacity(), 1.0);
        } else {
            QVERIFY2(corner.plate->opacity() < 1.0, qPrintable(QString::number(corner.plate->opacity())));
        }
        if (cornerWanted) {
            QCOMPARE(corner.modeSwitch->opacity(), 1.0);
        }
    }
    QVERIFY(animations.step(32));
    image = window->grabWindow();
    QVERIFY(!corner.notice->isVisible());
    // Nothing comes in again behind it: as the narrowing ended, the notice
    // showed for an instant and its text started to arrive (H17).
    for (int frame = 0; frame < 8; ++frame) {
        QVERIFY2(corner.notice->property("arrivalLevel").toDouble() == 1.0,
                 qPrintable(QStringLiteral("after it left, frame %1: %2")
                                .arg(frame).arg(corner.notice->property("arrivalLevel").toDouble())));
        QVERIFY(!corner.notice->isVisible());
        (void)animations.step(16);
    }
    if (zoomed) {
        QCOMPARE(sceneRectOf(corner.plate).left(), plainLeft);
        QCOMPARE(sceneRectOf(corner.reset), resetAt);
        QCOMPARE(corner.plate->opacity(), cornerWanted ? 1.0 : 0.0);
        if (cornerWanted) {
            // The plate as before the view was moved, with its own edge.
            QQuickItem *fill = itemNamed(corner.plate, QStringLiteral("plotPlateFill"));
            QVERIFY(fill != nullptr && fill->isVisible());
        }
    } else {
        QVERIFY(!corner.plate->isVisible());
    }
    QCOMPARE(corner.modeSwitch->opacity(), cornerWanted ? 1.0 : 0.0);
}

namespace {

// Gives the main window's plots `clock`, which the fallback bridge lacks,
// as the engine's, at `seconds` of the track of token 1, paused.
bool shareClock(QQuickWindow *window, PlaybackClock &clock, double seconds) {
    auto *bridge = window->property("uiBridge").value<QObject *>();
    if (bridge == nullptr || !bridge->setProperty("playbackClock", QVariant::fromValue(&clock))) return false;
    PlaybackSample sample;
    sample.positionSeconds = seconds;
    sample.sampledAtNs = clock.nowNs();
    sample.durationSeconds = 229.0;
    sample.trackToken = 1;
    clock.applySample(sample, false);
    return true;
}

// Zooms the corner's plot in to `level` and, for the spectrogram, gives its
// lanes the columns of that zoom, as the analysis does once it has
// restarted for it: until then a lane shows its canvas scaled, and its view
// stays the scaled canvas's.
bool zoomWithColumns(const PlotCorner &corner, double level) {
    corner.zoom(level);
    if (corner.mode != 0) return true;
    QList<SpectrogramItem *> lanes;
    collectQuickItems(corner.spectrogramSurface, &lanes);
    for (SpectrogramItem *lane : std::as_const(lanes)) {
        if (!QTest::qWaitFor([lane, level]() { return std::abs(lane->zoomLevel() - level) < 0.01; })) {
            return false;
        }
        int hop = 0;
        {
            QMutexLocker lock(&lane->m_stateMutex);
            hop = lane->columnHopForZoomLocked(lane->backendZoomForRequestLocked());
        }
        if (hop <= 0) return false;
        constexpr int bins = 16;
        const int columns = static_cast<int>(229.0 * 48'000 / hop) + 1;
        lane->feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns, 0, columns, 48'000, hop,
                                   true, true, 1, false);
        if (!QTest::qWaitFor([lane]() {
                QMutexLocker lock(&lane->m_stateMutex);
                return !lane->zoomPreviewWantedLocked();
            })) {
            return false;
        }
    }
    return true;
}

// Sets `clock` to `seconds` of the track of `token` (0 for the one it has),
// as the engine does: a seek, or with another token a track change.
void setSharedPosition(PlaybackClock &clock, double seconds, quint64 token = 0) {
    PlaybackSample sample;
    sample.positionSeconds = seconds;
    sample.sampledAtNs = clock.nowNs();
    sample.trackToken = token != 0 ? token : clock.state().trackToken;
    sample.durationSeconds = 229.0;
    clock.applySample(sample, false);
}

} // namespace

void TransportInputSmokeTest::plotsTellFollowingAgainInOneOrder_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("spectrogram") << 0;
    QTest::newRow("waveform") << 1;
}

// A moved view that follows the playhead again tells where it is now, then
// where it was, then that it follows, in either plot: the spectrogram told
// that it followed first and where it was a frame later, the waveform where
// it was first, so that the seek bar's band could not glide from one to the
// other without waiting for both (H15). Where it was, only in its own
// track: by Follow Playhead and a seek, not by Repeat-One or another track.
void TransportInputSmokeTest::plotsTellFollowingAgainInOneOrder() {
    QFETCH(int, mode);
    PlaybackClock clock;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(shareClock(corner.window, clock, 100.0));
    QObject *plot = mode == 0 ? static_cast<QObject *>(corner.spectrogramSurface)
                              : static_cast<QObject *>(corner.waveform);
    QVERIFY(zoomWithColumns(corner, 4.0));
    // What the plot tells, in its order, and where its view was each time.
    QStringList told;
    QList<double> from;
    const auto start = [plot]() { return plot->property("visibleStartSeconds").toDouble(); };
    auto *lane = qobject_cast<SpectrogramItem *>(
        itemNamed(corner.spectrogramSurface, QStringLiteral("spectrogramPaneItem")));
    QVERIFY(lane != nullptr);
    QList<QMetaObject::Connection> connections;
    if (mode == 0) {
        connections << QObject::connect(lane, &SpectrogramItem::visibleRangeChanged, [&] { told << QStringLiteral("range"); });
        connections << QObject::connect(lane, &SpectrogramItem::followedAgain, [&](double a, double) {
            told << QStringLiteral("from");
            from << a;
        });
        connections << QObject::connect(spectrogramFollow(corner.window), &SpectrogramViewFollow::detachedChanged,
                                        [&] { told << QStringLiteral("detached"); });
    } else {
        connections << QObject::connect(corner.waveform, &WaveformEditorItem::visibleRangeChanged,
                                        [&] { told << QStringLiteral("range"); });
        connections << QObject::connect(corner.waveform, &WaveformEditorItem::followedAgain, [&](double a, double) {
            told << QStringLiteral("from");
            from << a;
        });
        connections << QObject::connect(corner.waveform, &WaveformEditorItem::detachedChanged,
                                        [&] { told << QStringLiteral("detached"); });
    }
    // A frame of the moved view, so that the plot has told where it is.
    const auto moveView = [&]() -> double {
        if (!corner.moveView()) return -1.0;
        corner.window->grabWindow();
        QTest::qWait(30);
        corner.window->grabWindow();
        told.clear();
        from.clear();
        return start();
    };
    const auto onTheWay = [&]() {
        // The first of them as it follows again; a frame may tell more.
        const qsizetype detached = told.indexOf(QStringLiteral("detached"));
        return detached > 0 ? told.mid(0, detached + 1) : told;
    };

    // Follow Playhead.
    double moved = moveView();
    QVERIFY(moved >= 0.0);
    QVERIFY(QMetaObject::invokeMethod(corner.surface, "followPlayhead"));
    QCOMPARE(onTheWay(), (QStringList{QStringLiteral("range"), QStringLiteral("from"), QStringLiteral("detached")}));
    QCOMPARE(from, QList<double>{moved});
    QVERIFY2(std::abs(start() - moved) > 5.0, qPrintable(QString::number(start())));

    // A seek in the track, as from the seek bar.
    moved = moveView();
    QVERIFY(moved >= 0.0);
    setSharedPosition(clock, 150.0);
    QCOMPARE(onTheWay(), (QStringList{QStringLiteral("range"), QStringLiteral("from"), QStringLiteral("detached")}));
    QCOMPARE(from, QList<double>{moved});

    // Repeat-One: the track again, from its start, as a gapless successor
    // of itself; and another track.
    for (const double restart : {0.0, 30.0}) {
        moved = moveView();
        QVERIFY(moved >= 0.0);
        setSharedPosition(clock, restart, clock.state().trackToken + 1);
        QTRY_VERIFY(!corner.surface->property("detached").toBool());
        QVERIFY2(!told.contains(QStringLiteral("from")), qPrintable(told.join(u' ')));
    }
    for (const QMetaObject::Connection &c : std::as_const(connections)) QObject::disconnect(c);
}

void TransportInputSmokeTest::seekBarBandLandsWithThePlot_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("spectrogram") << 0;
    QTest::newRow("waveform") << 1;
}

// As a moved view follows the playhead again, by Follow Playhead or a seek,
// the seek bar's band is where the plot's view is in the frame the plot
// shows it in, and stays there, in either plot: it glided there over 120
// ms, behind a plot that had already landed, so the two moved differently
// for one change (H16). On Repeat-One, from the track's end back to its
// start, too.
void TransportInputSmokeTest::seekBarBandLandsWithThePlot() {
    QFETCH(int, mode);
    PlaybackClock clock;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(shareClock(corner.window, clock, 100.0));
    QQuickWindow *window = corner.window;
    auto *seek = window->findChild<QQuickItem *>(QStringLiteral("seekSlider"));
    auto *band = window->findChild<QQuickItem *>(QStringLiteral("seekViewBand"));
    QVERIFY(seek && band && band->parentItem());
    QObject *plot = mode == 0 ? static_cast<QObject *>(corner.spectrogramSurface)
                              : static_cast<QObject *>(corner.waveform);
    QTest::mouseMove(window, corner.away());
    QVERIFY(zoomWithColumns(corner, 4.0));
    QTRY_VERIFY(band->isVisible());
    const qreal ratio = window->effectiveDevicePixelRatio();
    // The band on the bar, and where the plot's view says it is.
    const auto bandAt = [band]() {
        const QRectF bar = sceneRectOf(band->parentItem());
        const QRectF drawn = sceneRectOf(band);
        return QPointF(drawn.left() - bar.left(), drawn.right() - bar.left());
    };
    const auto plotAt = [&]() {
        QVariant left;
        QVariant right;
        if (!QMetaObject::invokeMethod(seek, "barPixelOf", Q_RETURN_ARG(QVariant, left),
                                       Q_ARG(QVariant, plot->property("visibleStartSeconds")))
            || !QMetaObject::invokeMethod(seek, "barPixelOf", Q_RETURN_ARG(QVariant, right),
                                          Q_ARG(QVariant, plot->property("visibleEndSeconds")))) {
            return QPointF(-1.0, -1.0);
        }
        return QPointF(std::round(left.toDouble()) / ratio, std::round(right.toDouble()) / ratio);
    };
    const auto moveView = [&]() -> double {
        if (!corner.moveView()) return -1.0;
        // A frame of the moved view, which the spectrogram says it shows as
        // it draws it, and the events that come after it.
        window->grabWindow();
        QTest::qWait(50);
        window->grabWindow();
        return band->isVisible() ? bandAt().x() : -1.0;
    };
    // Frame by frame from the one the view comes back in: the band is
    // where the plot is, which is not where it was moved to.
    const auto landsWithThePlot = [&](SteppedAnimationDriver &animations, double movedLeft) {
        QString path;
        for (int frame = 0; frame <= 12; ++frame) {
            window->grabWindow();
            const QPointF at = bandAt();
            path += QString::number(at.x()) + QLatin1Char(' ');
            if (at != plotAt() || std::abs(at.x() - movedLeft) < 20.0) {
                return QStringLiteral("frame %1: band %2-%3, plot %4-%5; band %6")
                    .arg(frame).arg(at.x()).arg(at.y()).arg(plotAt().x()).arg(plotAt().y()).arg(path);
            }
            (void)animations.step(16);
        }
        return QString();
    };

    for (const bool bySeek : {false, true}) {
        const double movedLeft = moveView();
        QVERIFY(movedLeft >= 0.0);
        SteppedAnimationDriver animations;
        if (bySeek) {
            setSharedPosition(clock, 20.0);
        } else {
            QVERIFY(QMetaObject::invokeMethod(corner.surface, "followPlayhead"));
        }
        QVERIFY(!corner.surface->property("detached").toBool());
        const QString failure = landsWithThePlot(animations, movedLeft);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

    // Repeat-One from a view moved late in the track.
    setSharedPosition(clock, 200.0);
    const double movedLeft = moveView();
    QVERIFY(movedLeft >= 0.0);
    {
        SteppedAnimationDriver animations;
        setSharedPosition(clock, 0.0, clock.state().trackToken + 1);
        QTRY_VERIFY(!corner.surface->property("detached").toBool());
        const QString failure = landsWithThePlot(animations, movedLeft);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
}

// "Not following" and its divider come in over 100 ms, OutCubic, with
// the plate's reveal, rather than stand on it in a frame as it faded in
// (H15); and as the plate shows already, as a drag moves the view, once
// the plate holds them (notFollowingMovesAsOnePlate).
void TransportInputSmokeTest::notFollowingComesInWithThePlate() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, 0, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    for (const bool cornerWanted : {false, true}) {
        QTest::mouseMove(window, cornerWanted ? corner.onPane() : corner.away());
        corner.zoom(4.0);
        QTRY_COMPARE(corner.plate->opacity(), cornerWanted ? 1.0 : 0.0);
        window->grabWindow();
        SteppedAnimationDriver animations;
        QVERIFY(corner.moveView());
        QVERIFY(corner.moved());
        // On the plate, or in the room it grows by.
        QVERIFY(cornerWanted || (corner.text->isVisible() && corner.divider->isVisible()));
        window->grabWindow();
        QCOMPARE(corner.text->opacity(), 0.0);
        QCOMPARE(corner.divider->opacity(), 0.0);
        double level = 0.0;
        double gain = std::numeric_limits<double>::infinity();
        for (int frame = 1; frame * 16 < 100 && !cornerWanted; ++frame) {
            QVERIFY(animations.step(16));
            window->grabWindow();
            const double now = corner.text->opacity();
            QVERIFY2(now > level && now < 1.0, qPrintable(QStringLiteral("frame %1: %2").arg(frame).arg(now)));
            QVERIFY(now - level <= gain + 1e-9);
            QCOMPARE(corner.divider->opacity(), now);
            if (!cornerWanted) {
                // With the plate, on the same curve.
                QVERIFY2(std::abs(corner.plate->opacity() - now) < 1e-6,
                         qPrintable(QStringLiteral("%1 and %2").arg(corner.plate->opacity()).arg(now)));
            }
            gain = now - level;
            level = now;
        }
        // With the row showing, it comes in as the plate has grown for it.
        QVERIFY(animations.step(cornerWanted ? 128 : 16));
        QCOMPARE(corner.text->opacity(), 1.0);
        QCOMPARE(corner.divider->opacity(), 1.0);
        // It fades out as the plate narrows, and goes with the plate's fade
        // as the row fades out (notFollowingLeavesOneWay).
        QVERIFY(QMetaObject::invokeMethod(corner.surface, "followPlayhead"));
        QVERIFY(corner.text->isVisible());
        (void)animations.step(300);
        QVERIFY(!corner.notice->isVisible());
    }
}


void TransportInputSmokeTest::notFollowingArrivesAsItLeaves_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("zoomed");
    QTest::newRow("spectrogram, zoomed") << 0 << true;
    QTest::newRow("waveform, zoomed") << 1 << true;
    QTest::newRow("spectrogram, at the default zoom") << 0 << false;
}

// "Not following" comes as it goes while the corner's row shows, as the
// view is dragged away or autoscroll stops on it: zoomed, the plate grows
// to the left over 80 ms, OutCubic, Reset and the switch staying, and its
// left corners stay round; the text and divider fade in once it holds them
// (notFollowingMovesAsOnePlate). It jumped 89 px wider in a frame, an
// empty box, as the text faded in (H17). At the default zoom the plate
// fades in at its width over 120 ms, its text with it.
void TransportInputSmokeTest::notFollowingArrivesAsItLeaves() {
    QFETCH(int, mode);
    QFETCH(bool, zoomed);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    QTest::mouseMove(window, corner.onPane() + QPoint(1, 1));
    QTest::mouseMove(window, corner.onPane());
    corner.zoom(zoomed ? 4.0 : 1.0);
    QTRY_VERIFY(corner.layer->property("cornerWanted").toBool());
    QTRY_COMPARE(corner.modeSwitch->opacity(), 1.0);
    if (zoomed) {
        QTRY_COMPARE(corner.plate->opacity(), 1.0);
    }
    window->grabWindow();
    const QRectF resetAt = sceneRectOf(corner.reset);
    const QRectF switchAt = sceneRectOf(corner.modeSwitch);
    const double plainLeft = sceneRectOf(corner.plate).left();
    const qreal ratio = window->effectiveDevicePixelRatio();
    const auto pixel = [ratio](const QImage &image, double x, double y) {
        return image.pixelColor(static_cast<int>(std::floor(x * ratio)), static_cast<int>(std::floor(y * ratio)));
    };

    SteppedAnimationDriver animations;
    QVERIFY(corner.moveView());
    QVERIFY(corner.moved());
    QImage image = window->grabWindow();
    // The frame it is moved in: the plate as it was, nothing on it yet.
    const double room = corner.notice->implicitWidth() + 4;
    // How far it has come: the plate's growth, zoomed, and its fade
    // otherwise.
    const auto came = [&]() {
        return zoomed ? (plainLeft - sceneRectOf(corner.plate).left()) / room : corner.plate->opacity();
    };
    QVERIFY(!zoomed || corner.text->opacity() == 0.0 || !corner.notice->isVisible());
    QCOMPARE(came(), 0.0);
    if (zoomed) {
        QCOMPARE(sceneRectOf(corner.plate).left(), plainLeft);
        QCOMPARE(corner.plate->opacity(), 1.0);
    }
    double left = plainLeft;
    double step = std::numeric_limits<double>::infinity();
    double level = 0.0;
    for (int frame = 1; frame * 16 < (zoomed ? 80 : 120); ++frame) {
        QVERIFY(animations.step(16));
        image = window->grabWindow();
        const QString where = QStringLiteral("frame %1").arg(frame);
        const QRectF plate = sceneRectOf(corner.plate);
        const double now = plate.left();
        const double text = came();
        QVERIFY2(text > level && text < 1.0, qPrintable(QStringLiteral("%1: %2 after %3").arg(where).arg(text).arg(level)));
        QCOMPARE(corner.divider->opacity(), corner.text->opacity());
        level = text;
        QCOMPARE(sceneRectOf(corner.modeSwitch), switchAt);
        QCOMPARE(corner.modeSwitch->opacity(), 1.0);
        QCOMPARE(plate.right() + 8, switchAt.left());
        if (!zoomed) {
            // At its width, fading in with its text, which is whole.
            QCOMPARE(corner.text->opacity(), 1.0);
            QCOMPARE(corner.notice->width(), corner.notice->implicitWidth());
            if (frame == 1) left = now;
            QCOMPARE(now, left);
            continue;
        }
        QCOMPARE(corner.plate->opacity(), 1.0);
        QCOMPARE(sceneRectOf(corner.reset), resetAt);
        // Growing to the left, more slowly as it goes.
        QVERIFY2((frame == 1 ? now < left : now <= left) && now >= plainLeft - room - 1e-6,
                 qPrintable(QStringLiteral("%1: %2 after %3, to %4").arg(where).arg(now).arg(left).arg(plainLeft - room)));
        QVERIFY2(left - now <= step + 1, qPrintable(QStringLiteral("%1: %2 after %3").arg(where).arg(left - now).arg(step)));
        step = left - now;
        left = now;
        // Its left corner round, the plot showing there.
        const double y = plate.top() - 0.5;
        const QColor plot = pixel(image, now - 12, y);
        const QColor corner0 = pixel(image, now - 0.5, y);
        QVERIFY2(channelDistance(corner0, plot) <= 2,
                 qPrintable(QStringLiteral("%1: corner %2, plot %3").arg(where, corner0.name(), plot.name())));
    }
    QVERIFY(animations.step(zoomed ? 64 : 32));
    image = window->grabWindow();
    QCOMPARE(corner.text->opacity(), 1.0);
    QCOMPARE(corner.divider->opacity(), 1.0);
    QCOMPARE(corner.plate->opacity(), 1.0);
    QCOMPARE(corner.notice->width(), corner.notice->implicitWidth());
    if (zoomed) {
        QCOMPARE(sceneRectOf(corner.plate).left(), plainLeft - room);
        QCOMPARE(sceneRectOf(corner.reset), resetAt);
        // One plate again, with its own fill and edge.
        QQuickItem *fill = itemNamed(corner.plate, QStringLiteral("plotPlateFill"));
        QVERIFY(fill != nullptr && fill->isVisible());
    }
}

void TransportInputSmokeTest::notFollowingMovesAsOnePlate_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("way");
    QTest::addColumn<bool>("dark");
    const char *ways[] = {"comes", "goes with the row staying", "goes with the row's fade"};
    for (const bool dark : {false, true}) {
        for (const int mode : {0, 1}) {
            for (int way = 0; way < 3; ++way) {
                QTest::addRow("%s, %s, %s", mode == 0 ? "spectrogram" : "waveform", ways[way], dark ? "dark" : "light")
                    << mode << way << dark;
            }
        }
    }
}

// "Not following" comes and goes on the zoomed plate as one plate, frame by
// frame: its own fill and edge, one tone across it (a room fading beside
// the rest read as two tones with a seam, H18), Reset and the switch
// staying. The text and divider are never cut and never outside the plate,
// drawn only once it holds them: clipped to the growing plate, the text
// read "ing", "llowing", "t following" for 83 ms (H18). Coming, the plate
// grows over 80 ms, OutCubic; going with the row staying, the text fades
// out as the plate narrows over 90 ms, InOutCubic, and no empty room
// lingers: the text went in a frame while the empty plate narrowed for
// 60 ms (H18). With the row fading out, it goes at the plate's width with
// the row's fade.
void TransportInputSmokeTest::notFollowingMovesAsOnePlate() {
    QFETCH(int, mode);
    QFETCH(int, way);
    QFETCH(bool, dark);
    const QPalette before = QGuiApplication::palette();
    if (dark) {
        // Breeze Dark's window, view and text.
        QPalette palette = before;
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#202326")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#141618")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#fcfcfc")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#fcfcfc")));
        QGuiApplication::setPalette(palette);
    }
    const auto restorePalette = qScopeGuard([&]() { QGuiApplication::setPalette(before); });
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    auto *tokens = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(tokens != nullptr);
    QCOMPARE(tokens->property("themeIsDark").toBool(), dark);
    const bool rowStays = way != 2;
    const QPoint pointer = rowStays ? corner.onPane() : corner.away();
    QTest::mouseMove(window, pointer + QPoint(1, 1));
    QTest::mouseMove(window, pointer);
    corner.zoom(4.0);
    QTRY_COMPARE(corner.layer->property("cornerWanted").toBool(), rowStays);
    if (rowStays) {
        QTRY_COMPARE(corner.plate->opacity(), 1.0);
    }
    window->grabWindow();
    const QRectF resetAt = sceneRectOf(corner.reset);
    const QRectF switchAt = sceneRectOf(corner.modeSwitch);
    const double plainLeft = sceneRectOf(corner.plate).left();
    if (way != 0) {
        QVERIFY(corner.moveView());
        QTRY_COMPARE(corner.plate->opacity(), 1.0);
        QTRY_COMPARE(corner.text->opacity(), 1.0);
        window->grabWindow();
    }
    const double room = corner.notice->implicitWidth() + 4;
    QVERIFY(room > 40);

    const qreal ratio = window->effectiveDevicePixelRatio();
    const auto pixel = [ratio](const QImage &image, double x, double y) {
        return image.pixelColor(static_cast<int>(std::floor(x * ratio)), static_cast<int>(std::floor(y * ratio)));
    };
    // How much of the text and divider is drawn.
    const auto drawn = [&]() { return corner.notice->isVisible() ? corner.text->opacity() : 0.0; };
    SteppedAnimationDriver animations;
    if (way == 0) {
        QVERIFY(corner.moveView());
    } else {
        QVERIFY(QMetaObject::invokeMethod(corner.surface, "followPlayhead"));
    }
    double left = sceneRectOf(corner.plate).left();
    int emptyFrames = 0;
    int longestEmpty = 0;
    for (int frame = 0; frame * 16 <= 200; ++frame) {
        if (frame > 0) {
            (void)animations.step(16);
        }
        const QImage image = window->grabWindow();
        const QString where = QStringLiteral("at %1 ms").arg(frame * 16);
        const QRectF plate = sceneRectOf(corner.plate);
        // One plate with its own fill and edge, Reset and the switch in
        // their places.
        QQuickItem *fill = itemNamed(corner.plate, QStringLiteral("plotPlateFill"));
        QQuickItem *edge = itemNamed(corner.plate, QStringLiteral("plotPlateEdge"));
        QVERIFY(fill && edge);
        if (corner.plate->isVisible() && corner.plate->opacity() > 0) {
            QVERIFY2(fill->isVisible() && edge->isVisible() && fill->opacity() == 1.0 && edge->opacity() == 1.0,
                     qPrintable(where));
        }
        QCOMPARE(sceneRectOf(corner.reset), resetAt);
        QCOMPARE(sceneRectOf(corner.modeSwitch), switchAt);
        QCOMPARE(plate.right() + 8, switchAt.left());
        if (rowStays) {
            QCOMPARE(corner.plate->opacity(), 1.0);
        } else {
            QCOMPARE(corner.plate->opacity(), corner.modeSwitch->opacity());
            if (corner.plate->opacity() > 0) {
                QCOMPARE(plate.left(), plainLeft - room);
            }
        }
        // Growing or narrowing one way, at its left edge.
        if (way == 0) {
            QVERIFY2(plate.left() <= left && plate.left() >= plainLeft - room,
                     qPrintable(QStringLiteral("%1: %2 after %3").arg(where).arg(plate.left()).arg(left)));
        } else if (way == 1) {
            QVERIFY2(plate.left() >= left && plate.left() <= plainLeft,
                     qPrintable(QStringLiteral("%1: %2 after %3").arg(where).arg(plate.left()).arg(left)));
        }
        left = plate.left();
        // Never cut, and drawn only in the plate.
        const double level = drawn();
        for (const QQuickItem *item : {corner.plate, corner.notice, corner.text}) {
            QVERIFY2(!item->clip(), qPrintable(QStringLiteral("%1: %2 clips").arg(where, item->objectName())));
        }
        if (level > 0) {
            const QRectF text = boxOf(corner.text);
            const double textLeft = text.left() + corner.text->property("leftPadding").toDouble();
            const double textRight = textLeft + corner.text->property("contentWidth").toDouble();
            const QRectF divider = sceneRectOf(corner.divider);
            QVERIFY2(textLeft >= plate.left() + 1 && textRight <= divider.left() && divider.right() <= plate.right(),
                     qPrintable(QStringLiteral("%1: text %2-%3, divider %4, plate %5-%6 at %7")
                                    .arg(where).arg(textLeft).arg(textRight).arg(divider.left())
                                    .arg(plate.left()).arg(plate.right()).arg(level)));
        }
        // No room standing empty: while the text is gone, the plate is
        // narrower than the text within 3 frames, 48 ms.
        const double emptyRoom = plainLeft - plate.left();
        if (corner.plate->opacity() > 0 && level < 0.05 && emptyRoom > 4) {
            longestEmpty = std::max(longestEmpty, ++emptyFrames);
        } else {
            emptyFrames = 0;
        }
        // One tone across the plate, along its top inside its edge, where
        // nothing is drawn on it, and the plot beside it.
        if (corner.plate->opacity() >= 1.0 && plate.width() > 2 * 6) {
            const double y = plate.top() + 1.5;
            const QColor first = pixel(image, plate.left() + 5.5, y);
            for (double x = plate.left() + 5.5; x < plate.right() - 5; x += 1.0) {
                const QColor now = pixel(image, x, y);
                QVERIFY2(channelDistance(now, first) <= 2,
                         qPrintable(QStringLiteral("%1: %2 at %3, %4 at the plate's left")
                                        .arg(where, now.name()).arg(x).arg(first.name())));
            }
        }
        // Timing: the plate grown over 80 ms, narrowed over 90 ms.
        if (way == 0 && frame * 16 >= 80) {
            QCOMPARE(plate.left(), plainLeft - room);
        } else if (way == 1 && frame * 16 >= 96) {
            QCOMPARE(plate.left(), plainLeft);
        }
    }
    QVERIFY2(longestEmpty <= 3, qPrintable(QStringLiteral("%1 frames").arg(longestEmpty)));
    if (way == 0) {
        QCOMPARE(corner.text->opacity(), 1.0);
        QCOMPARE(corner.divider->opacity(), 1.0);
    } else {
        QVERIFY(!corner.notice->isVisible());
        QCOMPARE(sceneRectOf(corner.plate).left(), plainLeft);
        QCOMPARE(corner.notice->property("arrivalLevel").toDouble(), 1.0);
    }
}

void TransportInputSmokeTest::crosshairGivesWayToAutoscroll_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("spectrogram") << 0;
    QTest::newRow("waveform") << 1;
}

// While autoscroll runs, the plot's crosshair and its readouts fade out over
// 100 ms, OutCubic, as the lane heads' buttons do, and come back as it stops:
// its pointer steers the speed, and the time under it churned at every
// frame, and went and came as the pointer left the window and came back
// (H18). Nothing else says autoscroll runs (#655).
void TransportInputSmokeTest::crosshairGivesWayToAutoscroll() {
    QFETCH(int, mode);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    auto *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("showSpectrogramCrosshair", true);
    QList<QQuickItem *> plots;
    if (mode == 0) {
        plots = visibleItemsNamed(corner.spectrogramSurface, QStringLiteral("spectrogramPaneItem"));
    } else {
        plots = {corner.waveform};
    }
    QVERIFY(!plots.isEmpty());
    const auto all = [&](double value) {
        return std::all_of(plots.begin(), plots.end(), [&](QQuickItem *plot) {
            return std::abs(plot->property("crosshairOpacity").toDouble() - value) < 1e-6;
        });
    };
    QTest::mouseMove(window, corner.onPane() + QPoint(1, 1));
    QTest::mouseMove(window, corner.onPane());
    corner.zoom(4.0);
    QTRY_VERIFY(all(1.0));

    SteppedAnimationDriver animations;
    middleClickPlot(plots.first(), plots.first()->width() / 2);
    QVERIFY(corner.surface->property("autoscrolling").toBool());
    // Under way after a frame, over 100 ms, OutCubic.
    QVERIFY(animations.step(16));
    const double during = corner.layer->property("crosshairLevel").toDouble();
    QVERIFY2(during > 0.0 && during < 1.0, qPrintable(QString::number(during)));
    (void)animations.step(96);
    QVERIFY(all(0.0));
    // Gone wherever the pointer steers it, off the window and back.
    for (const QPoint at : {corner.onPane() + QPoint(60, 0), corner.away(), corner.onPane() + QPoint(-40, 10)}) {
        QTest::mouseMove(window, at + QPoint(1, 0));
        QTest::mouseMove(window, at);
        (void)animations.step(32);
        QVERIFY(corner.surface->property("autoscrolling").toBool());
        QVERIFY2(all(0.0), qPrintable(QStringLiteral("at %1,%2").arg(at.x()).arg(at.y())));
    }
    // Back as it stops, with the pointer on the plot.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!corner.surface->property("autoscrolling").toBool());
    QVERIFY(animations.step(16));
    const double coming = corner.layer->property("crosshairLevel").toDouble();
    QVERIFY2(coming > 0.0 && coming < 1.0, qPrintable(QString::number(coming)));
    (void)animations.step(96);
    QVERIFY(all(1.0));
}

void TransportInputSmokeTest::cornerHoldsWhileAutoscrollRuns_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("spectrogram") << 0;
    QTest::newRow("waveform") << 1;
}

// While autoscroll runs, the corner's row stays as the pointer leaves the
// pane for the window's edge, where autoscroll is fastest: it faded out
// there and back in as autoscroll stopped on the moved view, mid-gesture
// (H16). "Not following" stays absent while it runs (#655): the row shows
// what it showed with the pointer on the pane, and nothing more.
void TransportInputSmokeTest::cornerHoldsWhileAutoscrollRuns() {
    QFETCH(int, mode);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    QQuickItem *plot = corner.waveform;
    if (mode == 0) {
        plot = itemNamed(corner.spectrogramSurface, QStringLiteral("spectrogramPaneItem"));
        QVERIFY(plot != nullptr);
    }
    // A move onto the pane, wherever the last test left the pointer.
    QTest::mouseMove(window, corner.onPane() + QPoint(1, 1));
    QTest::mouseMove(window, corner.onPane());
    corner.zoom(4.0);
    QTRY_COMPARE(corner.plate->opacity(), 1.0);
    QTRY_COMPARE(corner.modeSwitch->opacity(), 1.0);
    window->grabWindow();

    // The lane under the pointer shows its head's buttons, but not while
    // autoscroll runs, whose pointer steers its speed across the lanes:
    // the buttons came and went as it crossed them (H17).
    const QList<QQuickItem *> heads = itemsNamed(corner.layer, QStringLiteral("plotLaneHead"));
    const auto laneHovered = [&]() {
        for (QQuickItem *head : heads) {
            if (head->property("laneHovered").toBool()) return true;
        }
        return false;
    };
    QVERIFY(!heads.isEmpty());
    QTRY_VERIFY(laneHovered());

    SteppedAnimationDriver animations;
    middleClickPlot(plot, plot->width() / 2);
    QVERIFY(corner.surface->property("autoscrolling").toBool());
    for (const double down : {0.2, 0.5, 0.8}) {
        const QPoint onLane =
            corner.layer->mapToScene(QPointF(corner.layer->width() / 2 + 20, corner.layer->height() * down)).toPoint();
        QTest::mouseMove(window, onLane + QPoint(1, 0));
        QTest::mouseMove(window, onLane);
        (void)animations.step(16);
        QVERIFY(corner.surface->property("autoscrolling").toBool());
        QVERIFY2(!laneHovered(), qPrintable(QStringLiteral("lane at %1,%2").arg(onLane.x()).arg(onLane.y())));
    }
    QTest::mouseMove(window, corner.away());
    for (int frame = 0; frame < 20; ++frame) {
        (void)animations.step(16);
        window->grabWindow();
        QVERIFY(corner.surface->property("autoscrolling").toBool());
        QVERIFY2(corner.plate->opacity() == 1.0 && corner.modeSwitch->opacity() == 1.0,
                 qPrintable(QStringLiteral("frame %1: plate %2, switch %3")
                                .arg(frame).arg(corner.plate->opacity()).arg(corner.modeSwitch->opacity())));
        QVERIFY(!corner.moved() && !corner.notice->isVisible());
    }
    // Stopped there, on the moved view: the row stays, and says so, the
    // plate growing to the left for it rather than in a frame (H17).
    const double before = sceneRectOf(corner.plate).left();
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!corner.surface->property("autoscrolling").toBool());
    QVERIFY(corner.moved());
    QCOMPARE(sceneRectOf(corner.plate).left(), before);
    double left = before;
    for (int frame = 0; frame < 10; ++frame) {
        (void)animations.step(16);
        const QImage image = window->grabWindow();
        QCOMPARE(corner.plate->opacity(), 1.0);
        QCOMPARE(corner.modeSwitch->opacity(), 1.0);
        const double now = sceneRectOf(corner.plate).left();
        QVERIFY2(frame == 0 ? now < before : now <= left,
                 qPrintable(QStringLiteral("frame %1: %2 after %3").arg(frame).arg(now).arg(left)));
        left = now;
    }
    QVERIFY(corner.notice->isVisible());
    QCOMPARE(left, before - (corner.notice->implicitWidth() + 4));
    QCOMPARE(corner.text->opacity(), 1.0);
}

void TransportInputSmokeTest::laneHeadsStayQuietThroughAPan_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("spectrogram") << 0;
    QTest::newRow("waveform") << 1;
}

// A middle-drag that pans the plot shows no lane's Mute and Solo, as
// autoscroll shows none: the hand holds the plot, not a lane's buttons,
// which opened under it as it went (H19).
void TransportInputSmokeTest::laneHeadsStayQuietThroughAPan() {
    QFETCH(int, mode);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, mode, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    QQuickItem *plot = corner.waveform;
    if (mode == 0) {
        plot = itemNamed(corner.spectrogramSurface, QStringLiteral("spectrogramPaneItem"));
        QVERIFY(plot != nullptr);
    }
    QTest::mouseMove(window, corner.onPane() + QPoint(1, 1));
    QTest::mouseMove(window, corner.onPane());
    corner.zoom(4.0);
    const QList<QQuickItem *> heads = itemsNamed(corner.layer, QStringLiteral("plotLaneHead"));
    const auto laneHovered = [&]() {
        for (QQuickItem *head : heads) {
            if (head->property("laneHovered").toBool()) return true;
        }
        return false;
    };
    QVERIFY2(!heads.isEmpty(), qPrintable(QString::number(heads.size())));
    QTRY_VERIFY(laneHovered());

    // From the upper lane, left and down across the plot's lanes.
    const QPointF from = corner.layer->mapToScene(
        QPointF(corner.layer->width() / 2, corner.layer->height() * 0.25));
    QTest::mouseMove(window, from.toPoint());
    QTRY_VERIFY(laneHovered());
    QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, from.toPoint());
    for (int step = 1; step <= 10; ++step) {
        const QPoint at = (from + QPointF(-12.0 * step, corner.layer->height() * 0.05 * step)).toPoint();
        QTest::mouseMove(window, at);
        QVERIFY2(plot->property("panning").toBool(), qPrintable(QStringLiteral("step %1").arg(step)));
        QVERIFY2(!laneHovered(), qPrintable(QStringLiteral("step %1, at %2,%3")
                                                .arg(step).arg(at.x()).arg(at.y())));
        QVERIFY(corner.surface->property("panning").toBool());
    }
    // Let go, the lane under the pointer has its buttons again.
    const QPoint end = (from + QPointF(-120.0, corner.layer->height() * 0.5)).toPoint();
    QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, end);
    QTRY_VERIFY(!corner.surface->property("panning").toBool());
    QTest::mouseMove(window, end + QPoint(1, 0));
    QTRY_VERIFY(laneHovered());
}

// "Not following" and the zoom readout on a plot's plate are drawn as the
// plate's other letters are, as curves with grey antialiasing (the
// platform's subpixel antialiasing fringed them in colour on the plate);
// without the plot's colours, in the window's chrome, as the window's
// labels are.
void TransportInputSmokeTest::zoomPlateTextIsDrawnAsThePlotPlatesIs() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components

Window {
    id: paletteRoot
    width: 400
    height: 200
    visible: true

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    Column {
        Components.PlotPlate {
            objectName: "lanePlate"
            plotColors: palette.spectrogramPlot
            uiPalette: palette
            text: "L"
        }
        Components.FollowNotice {
            objectName: "plotNotice"
            uiPalette: palette
            plotColors: palette.spectrogramPlot
            divided: true
        }
        Components.ZoomReadout {
            objectName: "plotReadout"
            uiPalette: palette
            plotColors: palette.waveformPlot
            zoomLevel: 2
        }
        Components.FollowNotice {
            objectName: "windowNotice"
            uiPalette: palette
        }
        Components.ZoomReadout {
            objectName: "windowReadout"
            uiPalette: palette
            zoomLevel: 2
        }
        Label {
            objectName: "windowLabel"
            text: "Window"
        }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    const auto renderTypeOf = [window](const QString &name, const QString &child = QString()) {
        QQuickItem *item = itemNamed(window->contentItem(), name);
        if (item != nullptr && !child.isEmpty()) item = itemNamed(item, child);
        return item != nullptr ? item->property("renderType").toInt() : -1;
    };
    const int curves = renderTypeOf(QStringLiteral("lanePlate"), QStringLiteral("plotPlateLabel"));
    QCOMPARE(curves, 2 /* Text.CurveRendering */);
    QCOMPARE(renderTypeOf(QStringLiteral("plotNotice"), QStringLiteral("followNoticeText")), curves);
    QCOMPARE(renderTypeOf(QStringLiteral("plotReadout")), curves);
    // The window's own, as the style draws its labels.
    const int windows = renderTypeOf(QStringLiteral("windowLabel"));
    QVERIFY(windows >= 0 && windows != curves);
    QCOMPARE(renderTypeOf(QStringLiteral("windowNotice"), QStringLiteral("followNoticeText")), windows);
    QCOMPARE(renderTypeOf(QStringLiteral("windowReadout")), windows);
}

// The divider between "Not following" and the zoom has 6 px on each side
// at every digit count: the readout is as wide as its text (as wide as
// its widest four-digit value, it had 18 px on the zoom's side at three
// digits, H15). The plate grows to the left, so Reset never moves, and
// "Not following" moves once, as the zoom reaches four digits.
void TransportInputSmokeTest::zoomPlateSpacesItsDividerEvenly() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    PlotCorner corner;
    const QString error = loadPlotCorner(engine, libraryModel, 0, corner);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = corner.window;
    QTest::mouseMove(window, corner.onPane());
    corner.zoom(3.81);
    QTRY_VERIFY(corner.readout->isVisible());
    QVERIFY(corner.moveView());
    QTRY_VERIFY(corner.text->isVisible());
    QTRY_COMPARE(corner.text->opacity(), 1.0);

    std::optional<QRectF> resetAt;
    std::optional<double> noticeAt;
    // Its digits, whatever the locale groups them with.
    const auto digits = [&corner]() {
        return corner.readout->property("text").toString().remove(QRegularExpression(QStringLiteral("[^0-9]")));
    };
    for (const auto &[level, shown] : std::initializer_list<std::pair<double, const char *>>{
             {3.81, "381"}, {9.99, "999"}, {10.0, "1000"}, {16.0, "1600"}, {1.25, "125"}}) {
        corner.zoom(level);
        if (!corner.moved()) QVERIFY(corner.moveView());
        QTRY_COMPARE(digits(), QString::fromLatin1(shown));
        QTRY_VERIFY(corner.text->isVisible());
        window->grabWindow();
        const double readoutText = boxOf(corner.readout).left() + corner.readout->property("leftPadding").toDouble();
        QCOMPARE(corner.readout->width(), corner.readout->property("implicitWidth").toDouble());
        QCOMPARE(sceneRectOf(corner.divider).left() - boxOf(corner.text).right(), 6.0);
        QCOMPARE(readoutText - sceneRectOf(corner.divider).right(), 6.0);
        if (!resetAt) resetAt = sceneRectOf(corner.reset);
        QCOMPARE(sceneRectOf(corner.reset), *resetAt);
        const double noticeLeft = boxOf(corner.text).left();
        if (!noticeAt) noticeAt = noticeLeft;
        if (level >= 10.0) {
            QVERIFY2(noticeLeft < *noticeAt, qPrintable(QStringLiteral("%1 at %2").arg(noticeLeft).arg(shown)));
        } else {
            QCOMPARE(noticeLeft, *noticeAt);
        }
    }
}

void TransportInputSmokeTest::movedViewSaysSoInTheViewers_data() {
    QTest::addColumn<int>("fullscreenMode");
    QTest::addColumn<bool>("strip");
    QTest::addColumn<int>("points");
    QTest::newRow("in the window") << 0 << true << 10;
    QTest::newRow("whole screen") << 1 << true << 10;
    QTest::newRow("whole screen without the strip") << 1 << false << 10;
    // The offscreen screen is 800 px wide: the strip has no room for all.
    QTest::newRow("whole screen, 24 pt") << 1 << true << 24;
}

// The viewers say the same in their own controls, which grow to the left
// and hide as they did (#467).
void TransportInputSmokeTest::movedViewSaysSoInTheViewers() {
    QFETCH(int, fullscreenMode);
    QFETCH(bool, strip);
    QFETCH(int, points);
    const ScopedApplicationFont font(points);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide = window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("visualizationSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge && viewer && autoHide && surface && waveformSurface);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(!surface->property("modeSwitching").toBool());
    bridge->setProperty("showFullscreenTitleStrip", strip);
    bridge->setProperty("viewerFullscreenMode", fullscreenMode);
    autoHide->setProperty("hideDelay", 600'000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *host = window;
    if (fullscreenMode == 1) {
        host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
        QVERIFY(host != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(host));
    }
    QTRY_COMPARE(waveform->window(), host);
    // Laid out there, for the drag and the click to have a width.
    QTRY_COMPARE(waveform->width(), waveformSurface->property("plotWidth").toDouble());
    QTRY_VERIFY(waveform->width() > 100.0);
    QQuickItem *controls = nullptr;
    QTRY_VERIFY((controls = visibleItemNamed(host, fullscreenMode == 0
                     ? QStringLiteral("spectrogramPopupControls")
                     : strip ? QStringLiteral("spectrogramFullscreenStrip")
                             : QStringLiteral("spectrogramFullscreenCorner"))) != nullptr);
    QQuickItem *notice = itemNamed(controls, QStringLiteral("viewerFollowNotice"));
    QQuickItem *reset = itemNamed(controls, QStringLiteral("viewerResetButton"));
    QQuickItem *readout = itemNamed(controls, QStringLiteral("viewerZoomReadout"));
    QVERIFY(notice && reset && readout);
    waveform->setZoomLevel(4.0);
    QTRY_VERIFY(reset->isVisible());
    QTRY_VERIFY(sceneRectOf(reset).right() <= host->width());
    host->grabWindow();
    QVERIFY(!notice->isVisible());
    const QRectF resetAt = sceneRectOf(reset);

    QVERIFY(detachWaveform(waveform));
    QTRY_VERIFY(notice->isVisible());
    QCOMPARE(sceneRectOf(reset), resetAt);
    QVERIFY(sceneRectOf(notice).right() <= sceneRectOf(readout).left());
    QVERIFY(sceneRectOf(controls).contains(sceneRectOf(notice)));
    QCOMPARE(itemNamed(notice, QStringLiteral("followNoticeText"))->property("color").value<QColor>(),
             readout->property("color").value<QColor>());

    if (fullscreenMode == 1 && strip) {
        // What the strip says of the track never runs under it.
        host->grabWindow();
        for (const char *name : {"viewerStripTitle", "viewerStripContext", "viewerStripTime"}) {
            QQuickItem *label = itemNamed(controls, QString::fromLatin1(name));
            QVERIFY(label != nullptr);
            // One with no room left, as at 24 pt in the test's 800 px
            // screen beside the strip's buttons, draws nothing.
            if (label->isVisible() && label->width() > 0) {
                QVERIFY2(boxOf(label).right() <= sceneRectOf(notice).left(), name);
            }
        }
    }

    middleClickPlot(waveform, waveform->width() / 2);
    QVERIFY(waveform->property("autoscrolling").toBool());
    QVERIFY(!notice->isVisible());
    QTest::keyClick(host, Qt::Key_Escape);
    QTRY_VERIFY(notice->isVisible());

    // The controls still hide at rest with it.
    autoHide->setProperty("hideDelay", 50);
    QVERIFY(QMetaObject::invokeMethod(autoHide, "wake"));
    QTRY_VERIFY(!notice->isVisible());
    QVERIFY(surface->property("viewMoved").toBool());
}

// View › Follow Playhead (F) brings the shown plot back to the playhead,
// playing or paused, in the main window and in both viewers. It is
// available only while the shown plot is detached: never for the
// spectrogram, which does not pan yet, and not while the album art viewer
// is open.
void TransportInputSmokeTest::followPlayheadBringsTheShownPlotBack() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *follow = window->findChild<QObject *>(QStringLiteral("followPlayheadAction"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && follow != nullptr && waveformSurface != nullptr);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    QCOMPARE(follow->property("shortcut").value<QKeySequence>(), QKeySequence(Qt::Key_F));
    QCOMPARE(follow->property("text").toString(), QStringLiteral("Follow &Playhead"));
    QSignalSpy triggered(follow, SIGNAL(triggered(QObject *)));
    const auto enabled = [follow]() { return follow->property("enabled").toBool(); };

    SpectrogramViewFollow *spectrogram = spectrogramFollow(window);
    QVERIFY(spectrogram != nullptr);

    // In `host`: off while the shown plot follows; F brings a detached
    // waveform or spectrogram back, playing or paused. A switch between
    // them brings the one left back to the playhead.
    const auto check = [&](QQuickWindow *host, const char *where) {
        for (const QString &state : {QStringLiteral("Playing"), QStringLiteral("Paused")}) {
            const QByteArray label = QByteArray(where) + ' ' + state.toUtf8();
            bridge->setProperty("playbackState", state);
            viewer->setProperty("visualizationMode", 1);
            QTRY_VERIFY2(waveform->isVisible(), label.constData());
            QVERIFY2(!waveform->detached() && !enabled(), label.constData());
            // Laid out in the host.
            QTRY_VERIFY2(waveform->width() > 100, label.constData());
            QVERIFY2(detachWaveform(waveform), label.constData());
            QTRY_VERIFY2(enabled(), label.constData());

            // The spectrogram shows at the playhead, and the waveform goes
            // back to it.
            viewer->setProperty("visualizationMode", 0);
            QTRY_VERIFY2(!enabled(), label.constData());
            QVERIFY2(!waveform->detached(), label.constData());
            // A centered spectrogram moved away comes back on F.
            spectrogram->detach(30.0, 0.5);
            QTRY_VERIFY2(enabled(), label.constData());
            const int before = triggered.count();
            QTest::keyClick(host, 'f');
            QTRY_VERIFY2(triggered.count() == before + 1, label.constData());
            QVERIFY2(!spectrogram->detached() && !enabled(), label.constData());
            // Moved away again, it goes back as the waveform shows.
            spectrogram->detach(30.0, 0.5);
            QTRY_VERIFY2(enabled(), label.constData());
            viewer->setProperty("visualizationMode", 1);
            QTRY_VERIFY2(!enabled(), label.constData());
            QVERIFY2(!spectrogram->detached(), label.constData());

            QVERIFY2(detachWaveform(waveform), label.constData());
            QTRY_VERIFY2(enabled(), label.constData());
            QTest::keyClick(host, 'f');
            QTRY_VERIFY2(triggered.count() == before + 2, label.constData());
            QVERIFY2(!waveform->detached(), label.constData());
            QVERIFY2(!enabled(), label.constData());
        }
        // Reset Zoom brings a moved spectrogram back too.
        bridge->setProperty("playbackState", QStringLiteral("Playing"));
        viewer->setProperty("visualizationMode", 0);
        QTRY_VERIFY2(!enabled(), where);
        auto *spectrogramSurface =
            window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
        QVERIFY(spectrogramSurface != nullptr);
        spectrogramSurface->setProperty(
            viewer->property("spectrogramViewerOpen").toBool() ? "_viewerZoomLevel" : "_widgetZoomLevel",
            2.0);
        spectrogram->detach(30.0, 0.5);
        QTRY_VERIFY2(enabled(), where);
        QObject *resetZoom = window->findChild<QObject *>(QStringLiteral("resetZoomAction"));
        QTRY_VERIFY2(resetZoom->property("enabled").toBool(), where);
        QVERIFY(QMetaObject::invokeMethod(resetZoom, "trigger"));
        QTRY_VERIFY2(!enabled() && !spectrogram->detached(), where);
        waveform->resetZoom();
    };

    // The queue has focus: a focused library takes F for its type-ahead.
    auto *queueView = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(queueView != nullptr);
    queueView->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(queueView->hasActiveFocus());
    check(window, "main window");

    // The album art viewer zooms the art: it leaves the key alone.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    viewer->setProperty("visualizationMode", 1);
    QVERIFY(detachWaveform(waveform));
    QTRY_VERIFY(enabled());
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY(!enabled());
    const int beforeAlbumArt = triggered.count();
    QTest::keyClick(window, 'f');
    QCoreApplication::processEvents();
    QCOMPARE(triggered.count(), beforeAlbumArt);
    QVERIFY(waveform->detached());
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY(enabled());
    waveform->followPlayhead();
    QTRY_VERIFY(!enabled());
    waveform->resetZoom();
    viewer->setProperty("visualizationMode", 0);

    // The viewer inside the main window.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    check(window, "viewer in the window");
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());

    // The viewer over the whole screen.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(waveformSurface->window() != nullptr && waveformSurface->window() != window);
    QQuickWindow *fullscreen = waveformSurface->window();
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    check(fullscreen, "viewer over the screen");
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

// While the shown plot is zoomed, the seek bar marks the part of the track
// its view shows with a band, the range the plot gives at its frames: the
// waveform's, following and panned, in the main window and in the viewer
// there, and the spectrogram's. None at either's default zoom, after Reset
// Zoom, or while the track's length is unknown; and the seek bar's value
// and what assistive technology reads of it stay as they were.
void TransportInputSmokeTest::seekBarMarksTheZoomedViewOfTheShownPlot() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *resetAction = window->findChild<QObject *>(QStringLiteral("resetZoomAction"));
    auto *transport = window->findChild<QQuickItem *>(QStringLiteral("transportBar"));
    auto *seek = window->findChild<QQuickItem *>(QStringLiteral("seekSlider"));
    auto *band = window->findChild<QQuickItem *>(QStringLiteral("seekViewBand"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge && viewer && resetAction && transport && seek && band && spectrogramSurface
            && waveformSurface);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(seek);
    QVERIFY(accessible != nullptr && accessible->valueInterface() != nullptr);
    const auto seekState = [&]() {
        return QVariantList{seek->property("value"), accessible->valueInterface()->currentValue(),
                            accessible->text(QAccessible::Name), accessible->text(QAccessible::Description)};
    };
    const QVariantList seekBefore = seekState();
    const auto bandIs = [transport, band](double start, double end) {
        return band->isVisible()
            && std::abs(transport->property("viewStartSeconds").toDouble() - start) < 1e-9
            && std::abs(transport->property("viewEndSeconds").toDouble() - end) < 1e-9;
    };
    const auto bandIsWaveforms = [&]() {
        return bandIs(waveform->visibleStartSeconds(), waveform->visibleEndSeconds());
    };
    // Where the band is on the bar.
    const auto bandOnBar = [&](double start, double end) {
        const QRectF bar = sceneRect(seek);
        const qreal ratio = window->effectiveDevicePixelRatio();
        const double pixels = std::floor(bar.width() * ratio + 1e-6);
        const QRectF drawn = sceneRect(band);
        return std::abs(drawn.left() - (bar.left() + std::round(start / 229.0 * pixels) / ratio)) < 1e-6
            && std::abs(drawn.right() - (bar.left() + std::round(end / 229.0 * pixels) / ratio)) < 1e-6;
    };

    // The waveform, once it has come in: its default zoom shows the whole
    // track.
    QObject *surface = window->findChild<QObject *>(QStringLiteral("visualizationSurface"));
    QVERIFY(surface != nullptr);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(surface->property("settledMode").toInt() == 1);
    QTRY_VERIFY(waveform->isVisible() && waveform->width() > 100);
    QTRY_VERIFY(waveform->visibleEndSeconds() == 229.0);
    QVERIFY(!band->isVisible());
    // Zoomed: the band shows the zoomed view at once.
    waveform->setZoomLevel(4.0);
    QVERIFY(bandIsWaveforms());
    QCOMPARE(waveform->visibleEndSeconds() - waveform->visibleStartSeconds(), 229.0 / 4.0);
    QVERIFY(bandOnBar(waveform->visibleStartSeconds(), waveform->visibleEndSeconds()));
    // Panned by a middle-drag.
    const double followingStart = waveform->visibleStartSeconds();
    QVERIFY(detachWaveform(waveform));
    QTRY_VERIFY(waveform->visibleStartSeconds() > followingStart + 229.0 / 8.0);
    QTRY_VERIFY(bandIsWaveforms());
    QVERIFY(bandOnBar(waveform->visibleStartSeconds(), waveform->visibleEndSeconds()));
    QCOMPARE(seekState(), seekBefore);
    // Reset Zoom: the zoom out waits for detail of the whole track, which
    // the fixture's track never gets, and the view stays zoomed meanwhile;
    // so does the band, though the zoom asked for is the default. It goes
    // as the whole track shows.
    QVERIFY(QMetaObject::invokeMethod(resetAction, "trigger"));
    QCOMPARE(waveform->zoomLevel(), 1.0);
    {
        QMutexLocker lock(&waveform->m_stateMutex);
        QVERIFY(waveform->m_zoomOutHandoffPending);
    }
    QVERIFY(bandIsWaveforms());
    QCOMPARE(waveform->visibleEndSeconds() - waveform->visibleStartSeconds(), 229.0 / 4.0);
    {
        QMutexLocker lock(&waveform->m_stateMutex);
        waveform->invalidateCacheLocked();
    }
    waveform->publishDetached();
    QCOMPARE(waveform->visibleEndSeconds() - waveform->visibleStartSeconds(), 229.0);
    QVERIFY(!band->isVisible());
    // Without a length.
    waveform->setZoomLevel(4.0);
    QTRY_VERIFY(bandIsWaveforms());
    bridge->setProperty("durationSeconds", std::numeric_limits<double>::quiet_NaN());
    QTRY_VERIFY(!band->isVisible());
    // The waveform keeps no zoom without a length: zoomed again once it
    // has one.
    bridge->setProperty("durationSeconds", 229.0);
    waveform->setZoomLevel(4.0);
    QTRY_VERIFY(bandIsWaveforms());

    // In the viewer in the window, as in the pane.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    waveform->setZoomLevel(8.0);
    QTRY_VERIFY(bandIsWaveforms()
                && std::abs(waveform->visibleEndSeconds() - waveform->visibleStartSeconds() - 229.0 / 8.0) < 1e-9);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    waveform->resetZoom();
    QTRY_VERIFY(!band->isVisible());

    // The centered spectrogram, with a track's columns. (A rolling view at
    // the track's start shows only the time before it.)
    bridge->setProperty("spectrogramDisplayMode", 1);
    viewer->setProperty("visualizationMode", 0);
    QList<QQuickItem *> panes;
    QTRY_VERIFY(!(panes = itemsNamed(spectrogramSurface, QStringLiteral("spectrogramPaneItem"))).isEmpty());
    auto *spectrogram = qobject_cast<SpectrogramItem *>(panes.constFirst());
    QVERIFY(spectrogram != nullptr);
    feedColumnStripes(*spectrogram);
    QTRY_VERIFY(std::isfinite(spectrogram->visibleStartSeconds()));
    QVERIFY(!band->isVisible());
    const double defaultZoom = spectrogramSurface->property("defaultZoomLevel").toDouble();
    spectrogramSurface->setProperty("_widgetZoomLevel", 2.0 * defaultZoom);
    QTRY_VERIFY(bandIs(spectrogram->visibleStartSeconds(), spectrogram->visibleEndSeconds()));
    QVERIFY(bandOnBar(spectrogram->visibleStartSeconds(), spectrogram->visibleEndSeconds()));
    QCOMPARE(seekState(), seekBefore);
    // Through a switch to the waveform, at its default zoom: the
    // spectrogram's band while it is the plot that shows, until the
    // waveform has come in.
    const auto spectrogramStillShows = [surface]() {
        return surface->property("settledMode").toInt() == 0
            && surface->property("modeFade").toDouble() <= 0.0;
    };
    viewer->setProperty("visualizationMode", 1);
    QVERIFY(!spectrogramStillShows()
            || bandIs(spectrogram->visibleStartSeconds(), spectrogram->visibleEndSeconds()));
    QTRY_VERIFY(surface->property("settledMode").toInt() == 1);
    QVERIFY(!band->isVisible());
    viewer->setProperty("visualizationMode", 0);
    QTRY_VERIFY(bandIs(spectrogram->visibleStartSeconds(), spectrogram->visibleEndSeconds()));
    QVERIFY(QMetaObject::invokeMethod(resetAction, "trigger"));
    QTRY_VERIFY(!band->isVisible());
    QCOMPARE(seekState(), seekBefore);
}

// A focused library takes the letters typed for its type-ahead, F too,
// before the shortcuts see them; Space stays Play or Pause, and keys with a
// modifier stay the shortcuts'. Anywhere else, F follows the playhead.
void TransportInputSmokeTest::fTypesInTheLibraryAndFollowsThePlayheadElsewhere() {
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
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *follow = window->findChild<QObject *>(QStringLiteral("followPlayheadAction"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *libraryView = qvariant_cast<QQuickItem *>(window->property("libraryViewRef"));
    QVERIFY(bridge != nullptr && viewer != nullptr && follow != nullptr && playPause != nullptr
            && library != nullptr && waveformSurface != nullptr && libraryView != nullptr);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveform->isVisible());
    QSignalSpy followed(follow, SIGNAL(triggered(QObject *)));
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject *)));

    QVERIFY(detachWaveform(waveform));
    QTRY_VERIFY(follow->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(libraryView, "forceActiveFocus"));
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(library, "selectIndex", Q_ARG(QVariant, 0)));

    // F, and Shift+F, go to the type-ahead.
    QTest::keyClick(window, 'f');
    QTest::keyClick(window, 'F', Qt::ShiftModifier);
    QCOMPARE(library->property("typeAheadBuffer").toString(), QStringLiteral("ff"));
    QCOMPARE(followed.count(), 0);
    QVERIFY(waveform->detached());
    // Space still plays or pauses.
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(toggled.count(), 1);
    // A shortcut with a modifier still works: Ctrl+0 resets the zoom, which
    // follows the playhead again.
    QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
    QTRY_COMPARE(waveform->zoomLevel(), 1.0);
    QVERIFY(!waveform->detached());
    QCOMPARE(followed.count(), 0);

    // Elsewhere, as in the queue, F follows the playhead.
    QVERIFY(detachWaveform(waveform));
    auto *queueView = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(queueView != nullptr);
    queueView->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(queueView->hasActiveFocus());
    QTest::keyClick(window, 'f');
    QTRY_COMPARE(followed.count(), 1);
    QVERIFY(!waveform->detached());
}

namespace {

// A wheel notch up at `point` in `host`, as the pointer sends it.
void wheelAt(QQuickWindow *host, QPointF point) {
    QWheelEvent wheel(point, host->mapToGlobal(point), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(host, &wheel);
}

// A middle-drag in `host` across `plot`, from right of its middle to left of
// it, as the pointer makes it.
void middleDragAcross(QQuickWindow *host, QQuickItem *plot) {
    const QRectF rect = sceneRectOf(plot);
    const int y = int(rect.center().y());
    const int from = int(rect.center().x() + rect.width() / 4);
    const int to = int(rect.center().x() - rect.width() / 4);
    QTest::mousePress(host, Qt::MiddleButton, Qt::NoModifier, QPoint(from, y));
    QTest::mouseMove(host, QPoint((from + to) / 2, y));
    QTest::mouseMove(host, QPoint(to, y));
    QTest::mouseRelease(host, Qt::MiddleButton, Qt::NoModifier, QPoint(to, y));
}

} // namespace

// The library has the focus at startup and keeps typed letters, F too, for
// its type-ahead. Using the plot, with any button or the wheel, takes the
// focus from it, so that F right after a middle-drag follows the playhead;
// the window's shortcuts work from there. A click on the library gives it
// the focus back.
void TransportInputSmokeTest::usingThePlotTakesTheFocusFromTheLibrary() {
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
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *follow = window->findChild<QObject *>(QStringLiteral("followPlayheadAction"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *libraryView = qvariant_cast<QQuickItem *>(window->property("libraryViewRef"));
    auto *plotFocus = window->findChild<QQuickItem *>(QStringLiteral("spectrogramPaneFocus"));
    QVERIFY(bridge != nullptr && viewer != nullptr && follow != nullptr && playPause != nullptr
            && library != nullptr && waveformSurface != nullptr && spectrogramSurface != nullptr
            && libraryView != nullptr && plotFocus != nullptr);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    // Out of the Tab order.
    QVERIFY(!plotFocus->activeFocusOnTab());
    setPlayingTrack(bridge);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveform->isVisible());
    QTRY_VERIFY(waveform->width() > 100);
    waveform->setZoomLevel(4.0);
    QSignalSpy followed(follow, SIGNAL(triggered(QObject *)));
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject *)));
    QSignalSpy seeked(waveform, &WaveformEditorItem::seekRequested);
    const auto focusLibrary = [&]() {
        return QMetaObject::invokeMethod(libraryView, "forceActiveFocus")
            && QTest::qWaitFor([&]() { return libraryView->hasActiveFocus(); });
    };
    const auto plotHasFocus = [&]() { return window->activeFocusItem() == plotFocus; };
    const QPoint plotPoint = sceneRectOf(waveformSurface).center().toPoint();

    // A middle-drag with the library focused, then F: the view follows again.
    QVERIFY(focusLibrary());
    QVERIFY(QMetaObject::invokeMethod(library, "selectIndex", Q_ARG(QVariant, 0)));
    middleDragAcross(window, waveformSurface);
    QVERIFY(waveform->detached());
    QTRY_VERIFY(follow->property("enabled").toBool());
    QTest::keyClick(window, 'f');
    QTRY_COMPARE(followed.count(), 1);
    QVERIFY(!waveform->detached());
    QCOMPARE(library->property("typeAheadBuffer").toString(), QString());
    QVERIFY(plotHasFocus());
    QVERIFY(!libraryView->hasActiveFocus());

    // From the plot, Space plays or pauses, and Ctrl+0 resets the zoom.
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(toggled.count(), 1);
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(toggled.count(), 2);
    middleDragAcross(window, waveformSurface);
    QVERIFY(waveform->detached());
    QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
    QTRY_COMPARE(waveform->zoomLevel(), 1.0);
    QVERIFY(!waveform->detached());
    QVERIFY(plotHasFocus());

    // A middle click on the plot starts autoscroll, which F stops, and a
    // switch to the spectrogram too, bringing the view back to the playhead.
    waveform->setZoomLevel(4.0);
    QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, plotPoint);
    QVERIFY(waveform->m_autoscroll.active() && waveform->detached());
    QTest::keyClick(window, 'f');
    QTRY_COMPARE(followed.count(), 2);
    QVERIFY(!waveform->m_autoscroll.active() && !waveform->detached());
    QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, plotPoint);
    QVERIFY(waveform->m_autoscroll.active());
    viewer->setProperty("visualizationMode", 0);
    QTRY_VERIFY(!waveform->m_autoscroll.active());
    QVERIFY(!waveform->detached());
    QVERIFY(QGuiApplication::overrideCursor() == nullptr);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveform->isVisible());

    // A click on the library gives it the focus back: F types there, and
    // leaves a detached view as it is.
    waveform->setZoomLevel(4.0);
    middleDragAcross(window, waveformSurface);
    QVERIFY(waveform->detached());
    QQuickItem *row = nullptr;
    QVERIFY(QMetaObject::invokeMethod(libraryView, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, 2)));
    QVERIFY(row != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRectOf(row).center().toPoint());
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QTest::keyClick(window, 'f');
    QCOMPARE(library->property("typeAheadBuffer").toString(), QStringLiteral("f"));
    QCOMPARE(followed.count(), 2);
    QVERIFY(waveform->detached());

    // A right-click seek on the plot takes the focus too.
    bridge->setProperty("showSpectrogramCrosshair", true);
    QTRY_VERIFY(waveform->crosshairEnabled());
    QVERIFY(focusLibrary());
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, plotPoint);
    QTRY_VERIFY(plotHasFocus());
    QCOMPARE(seeked.count(), 1);

    // So does a wheel zoom.
    QVERIFY(focusLibrary());
    const double zoomBefore = waveform->zoomLevel();
    wheelAt(window, plotPoint);
    QTRY_VERIFY(plotHasFocus());
    QTRY_VERIFY(waveform->zoomLevel() > zoomBefore);

    // And a press on the spectrogram.
    viewer->setProperty("visualizationMode", 0);
    QTRY_VERIFY(spectrogramSurface->isEnabled() && spectrogramSurface->opacity() > 0);
    QVERIFY(focusLibrary());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      sceneRectOf(spectrogramSurface).center().toPoint());
    QTRY_VERIFY(plotHasFocus());
    QVERIFY(!viewer->property("spectrogramViewerOpen").toBool());

    // A click on a button of the plot's, which has the focus from Tab,
    // leaves the focus there: Space presses it again.
    QQuickItem *waveformButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QVERIFY(waveformButton != nullptr);
    waveformButton->forceActiveFocus(Qt::TabFocusReason);
    QVERIFY(waveformButton->hasActiveFocus());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      sceneRectOf(waveformButton).center().toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QVERIFY(waveformButton->hasActiveFocus());
    const int toggledBefore = toggled.count();
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(toggled.count(), toggledBefore);
    // A press on the plot takes the focus from it.
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, plotPoint);
    QTRY_VERIFY(plotHasFocus());
}

// In the visualization viewer, a press on the plot leaves the focus with
// the viewer, as before: its keys, Tab through its controls, F and Escape
// work from there, in the window and over the whole screen.
void TransportInputSmokeTest::viewerKeysWorkAfterAPressOnItsPlot() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *follow = window->findChild<QObject *>(QStringLiteral("followPlayheadAction"));
    QQuickItem *pane = itemNamed(window->contentItem(), QStringLiteral("spectrogramPane"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *plotFocus = window->findChild<QQuickItem *>(QStringLiteral("spectrogramPaneFocus"));
    QVERIFY(bridge != nullptr && viewer != nullptr && follow != nullptr && pane != nullptr
            && waveformSurface != nullptr && plotFocus != nullptr);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveform->isVisible());
    QSignalSpy followed(follow, SIGNAL(triggered(QObject *)));

    for (const int fullscreenMode : {1, 0}) {
        const QByteArray where = QByteArray("fullscreen ") + QByteArray::number(fullscreenMode);
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
        QQuickWindow *host = fullscreenMode == 1
            ? viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow")) : window;
        QVERIFY2(host != nullptr, where.constData());
        QVERIFY(QTest::qWaitForWindowActive(host));
        QTRY_VERIFY2(!pane->isAncestorOf(waveformSurface), where.constData());
        QTRY_VERIFY2(waveform->width() > 100, where.constData());
        QQuickItem *viewFocus = host->findChild<QQuickItem *>(
            fullscreenMode == 1 ? QStringLiteral("spectrogramFullscreenViewFocus")
                                : QStringLiteral("spectrogramPopupViewFocus"));
        QQuickItem *close = visibleItemNamed(host, fullscreenMode == 1
            ? QStringLiteral("spectrogramFullscreenCloseButton")
            : QStringLiteral("spectrogramPopupCloseButton"));
        QVERIFY2(viewFocus != nullptr && close != nullptr, where.constData());

        // A middle-drag on the plot: the viewer's view has the focus, not
        // the pane's, and F follows the playhead.
        waveform->setZoomLevel(4.0);
        middleDragAcross(host, waveformSurface);
        QVERIFY2(waveform->detached(), where.constData());
        QTRY_VERIFY2(host->activeFocusItem() == viewFocus, where.constData());
        QVERIFY2(!plotFocus->hasActiveFocus(), where.constData());
        const int before = followed.count();
        QTest::keyClick(host, 'f');
        QTRY_VERIFY2(followed.count() == before + 1, where.constData());
        QVERIFY2(!waveform->detached(), where.constData());

        // Tab reaches the viewer's controls, and Space presses the one
        // focused, as before.
        QTest::mouseClick(host, Qt::RightButton, Qt::NoModifier,
                          sceneRectOf(waveformSurface).center().toPoint());
        QTRY_VERIFY2(host->activeFocusItem() == viewFocus, where.constData());
        QVERIFY2(tabTo(host, close), where.constData());
        QTest::keyClick(host, Qt::Key_Space);
        QTRY_VERIFY2(!viewer->property("spectrogramViewerOpen").toBool(), where.constData());

        // Escape closes it after a press on the plot. While autoscroll,
        // after a middle click, runs, Escape stops that and leaves the
        // viewer open.
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
        QVERIFY(QTest::qWaitForWindowActive(host));
        QTRY_VERIFY2(!pane->isAncestorOf(waveformSurface), where.constData());
        QTest::mouseClick(host, Qt::RightButton, Qt::NoModifier,
                          sceneRectOf(waveformSurface).center().toPoint());
        QTRY_VERIFY2(host->activeFocusItem() == viewFocus, where.constData());
        QTest::mouseClick(host, Qt::MiddleButton, Qt::NoModifier,
                          sceneRectOf(waveformSurface).center().toPoint());
        QVERIFY2(waveform->m_autoscroll.active() && waveform->detached(), where.constData());
        QTest::keyClick(host, Qt::Key_Escape);
        QVERIFY2(!waveform->m_autoscroll.active(), where.constData());
        QVERIFY2(viewer->property("spectrogramViewerOpen").toBool(), where.constData());
        QTest::keyClick(host, Qt::Key_Escape);
        QTRY_VERIFY2(!viewer->property("spectrogramViewerOpen").toBool(), where.constData());
        waveform->resetZoom();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

// A pan's closed hand is the pointer the windows show, in the pane and in
// both viewers, whose areas over the plot take the press for the focus; a
// viewer's pointer still hides with its controls, over either plot.
void TransportInputSmokeTest::panShowsAClosedHandInThePaneAndTheViewers() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide = window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QQuickItem *pane = itemNamed(window->contentItem(), QStringLiteral("spectrogramPane"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge && viewer && autoHide && pane && surface);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        surface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveform->isVisible() && waveform->width() > 100);

    // -1 is the pane.
    for (const int fullscreenMode : {-1, 0, 1}) {
        const QByteArray where = QByteArray("fullscreen ") + QByteArray::number(fullscreenMode);
        QQuickWindow *host = window;
        if (fullscreenMode >= 0) {
            autoHide->setProperty("hideDelay", 600000);
            bridge->setProperty("viewerFullscreenMode", fullscreenMode);
            QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
            QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
            if (fullscreenMode == 1) {
                host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
                QVERIFY(host != nullptr);
            }
            QVERIFY(QTest::qWaitForWindowActive(host));
            QTRY_VERIFY2(!pane->isAncestorOf(surface) && waveform->width() > 100, where.constData());
        }
        waveform->setZoomLevel(4.0);
        const QPoint from = sceneRectOf(surface).center().toPoint();
        QTest::mouseMove(host, from);
        QTest::mousePress(host, Qt::MiddleButton, Qt::NoModifier, from);
        QTest::mouseMove(host, from - QPoint(3, 0));
        QVERIFY2(host->cursor().shape() == Qt::ArrowCursor, where.constData());
        QTest::mouseMove(host, from - QPoint(60, 0));
        QVERIFY2(host->cursor().shape() == Qt::ClosedHandCursor, where.constData());
        QTest::mouseRelease(host, Qt::MiddleButton, Qt::NoModifier, from - QPoint(60, 0));
        QVERIFY2(host->cursor().shape() == Qt::ArrowCursor, where.constData());
        waveform->resetZoom();
        if (fullscreenMode < 0) continue;

        for (const int mode : {1, 0}) {
            viewer->setProperty("visualizationMode", mode);
            QVERIFY(QMetaObject::invokeMethod(autoHide, "wake"));
            QTest::mouseMove(host, from + QPoint(5 + mode, 0));
            autoHide->setProperty("hideDelay", 1);
            QTRY_VERIFY2(host->cursor().shape() == Qt::BlankCursor, where.constData());
            // The pointer moving brings it back.
            autoHide->setProperty("hideDelay", 600000);
            QTest::mouseMove(host, from + QPoint(40 + mode, 10));
            QTRY_VERIFY2(host->cursor().shape() == Qt::ArrowCursor, where.constData());
        }
        viewer->setProperty("visualizationMode", 1);
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
        QTRY_VERIFY2(pane->isAncestorOf(surface), where.constData());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY2(waveform->isVisible(), where.constData());
    }
}

// Reset is one control in the viewers, windowed and in the title strip: the
// zoom-original glyph in a button as wide as Close, named and explained by
// its tooltip (the strip said "Reset" in a word beside the window's glyph).
void TransportInputSmokeTest::viewerResetIsTheSameGlyphWindowedAndInTheStrip() {
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
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *art = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge && viewer && spectrogram && art);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    QAccessible::setActive(true);
    const QScopeGuard stopAccessibility([]() { QAccessible::setActive(false); });

    for (const bool albumArt : {false, true}) {
        for (const int fullscreenMode : {0, 1}) {
            const QString where = QStringLiteral("album art %1, fullscreen %2").arg(albumArt).arg(fullscreenMode);
            bridge->setProperty("viewerFullscreenMode", fullscreenMode);
            QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "openAlbumArtViewer" : "openSpectrogramViewer"));
            QQuickItem *view = albumArt ? art : spectrogram;
            QQuickWindow *host = window;
            if (fullscreenMode == 1) {
                QTRY_VERIFY(view->window() != nullptr && view->window() != window);
                host = view->window();
                QVERIFY(QTest::qWaitForWindowExposed(host));
            }
            if (albumArt) {
                art->setProperty("zoom", 2.0);
            } else {
                QTRY_VERIFY(spectrogram->property("viewerMode").toBool());
                spectrogram->setProperty("_viewerZoomLevel", 2.0);
            }
            const QString closeName = albumArt
                ? (fullscreenMode ? QStringLiteral("albumArtFullscreenCloseButton")
                                  : QStringLiteral("albumArtPopupCloseButton"))
                : (fullscreenMode ? QStringLiteral("spectrogramFullscreenCloseButton")
                                  : QStringLiteral("spectrogramPopupCloseButton"));
            QQuickItem *reset = nullptr;
            QTRY_VERIFY2((reset = visibleItemNamed(host, QStringLiteral("viewerResetButton"))) != nullptr
                             || (reset = visibleItemNamed(host, QStringLiteral("plotZoomResetButton"))) != nullptr,
                         qPrintable(where));
            QQuickItem *close = visibleItemNamed(host, closeName);
            QVERIFY2(close != nullptr, qPrintable(where));
            QVERIFY2(reset->property("display").toInt() == 0, qPrintable(where)); // AbstractButton.IconOnly
            QCOMPARE(QQmlProperty(reset, QStringLiteral("icon.name")).read().toString(),
                     QStringLiteral("zoom-original"));
            QCOMPARE(reset->size(), close->size());
            const QString tip = albumArt
                ? (fullscreenMode ? QStringLiteral("Fit the Image to the Screen")
                                  : QStringLiteral("Fit the Image to the Window"))
                : QStringLiteral("Reset Zoom");
            QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(reset);
            QVERIFY(accessible != nullptr);
            QCOMPARE(accessible->text(QAccessible::Name), tip);
            // The tooltip is the button's toolTipText, or its name without one.
            const QVariant toolTip = reset->property("toolTipText");
            QCOMPARE(toolTip.isValid() ? toolTip.toString() : reset->property("text").toString(), tip);

            if (albumArt) {
                art->setProperty("zoom", 1.0);
            } else {
                spectrogram->setProperty("_viewerZoomLevel", 0.0);
            }
            QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "closeAlbumArtViewer" : "closeSpectrogramViewer"));
            QTRY_VERIFY(!viewer->property(albumArt ? "albumArtViewerOpen" : "spectrogramViewerOpen").toBool());
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
    }
}

// The visualization viewer has one toolbar, windowed, in the title strip,
// and on the plot with the strip turned off: [zoom] [warning] [modes]
// [close], the chosen mode the plot's text at 18 % in its edge (in the
// window it was a slab of 35 to 44 %), the other's glyph whole (in the
// window at 72 %), and the warning in one colour (in the window it was the
// plot's text, right of the modes). A new warning moves neither the modes
// nor Close. On both plots, rolling and centred.
void TransportInputSmokeTest::viewerToolbarIsOneVocabularyWindowedAndInTheStrip() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide = window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(bridge && palette && viewer && autoHide && spectrogram && waveformSurface && layer);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    autoHide->setProperty("hideDelay", 600000);

    // A shown button by its name, whichever component draws it.
    const auto buttonNamed = [](QQuickWindow *host, const QString &text) -> QQuickItem * {
        QList<QQuickItem *> items;
        collectQuickItems(host->contentItem(), &items);
        for (QQuickItem *item : items) {
            if (item->isVisible() && item->property("checked").isValid() && item->property("text").toString() == text) {
                return item;
            }
        }
        return nullptr;
    };
    const auto partOf = [](QQuickItem *button, const QString &name) {
        QQuickItem *part = itemNamed(button, QStringLiteral("viewerToolButton") + name);
        return part != nullptr ? part : itemNamed(button, QStringLiteral("plotToolButton") + name);
    };
    // Where the modes and Close are from the window's right, by the case.
    QHash<QString, QList<double>> withoutWarning;
    for (const bool warning : {false, true}) {
        if (warning) {
            QVERIFY(QMetaObject::invokeMethod(bridge, "bridgeError", Q_ARG(QString, QStringLiteral("It failed.")),
                                              Q_ARG(int, 1), Q_ARG(int, 0),
                                              Q_ARG(QString, QStringLiteral("/music/bad.flac")),
                                              Q_ARG(QString, QString())));
        }
        struct Presentation {
            int fullscreenMode;
            bool strip;
        };
        for (const Presentation &shown : {Presentation{0, true}, Presentation{1, true}, Presentation{1, false}}) {
            bridge->setProperty("viewerFullscreenMode", shown.fullscreenMode);
            bridge->setProperty("showFullscreenTitleStrip", shown.strip);
            const bool closeShown = shown.fullscreenMode == 0 || shown.strip;
            QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
            QTRY_VERIFY(spectrogram->property("viewerMode").toBool());
            QQuickWindow *host = window;
            if (shown.fullscreenMode == 1) {
                QTRY_VERIFY(spectrogram->window() != nullptr && spectrogram->window() != window);
                host = spectrogram->window();
                QVERIFY(QTest::qWaitForWindowExposed(host));
            }
            // Off every button, so none is drawn hovered.
            QTest::mouseMove(host, QPoint(host->width() / 2, host->height() / 2));
            QVERIFY(QMetaObject::invokeMethod(autoHide, "wake"));
            for (const int displayMode : {0, 1}) {
                bridge->setProperty("spectrogramDisplayMode", displayMode);
                for (const int mode : {0, 1}) {
                    viewer->setProperty("visualizationMode", mode);
                    const QString where = QStringLiteral("fullscreen %1, strip %2, display %3, mode %4")
                                              .arg(shown.fullscreenMode).arg(shown.strip).arg(displayMode).arg(mode);
                    const QString chosenName = mode == 0 ? QStringLiteral("Spectrogram") : QStringLiteral("Waveform");
                    const QString otherName = mode == 0 ? QStringLiteral("Waveform") : QStringLiteral("Spectrogram");
                    QQuickItem *chosen = nullptr;
                    QQuickItem *other = nullptr;
                    QTRY_VERIFY2((chosen = buttonNamed(host, chosenName)) != nullptr
                                     && (other = buttonNamed(host, otherName)) != nullptr
                                     && chosen->property("checked").toBool(),
                                 qPrintable(where));
                    QObject *plot = qvariant_cast<QObject *>(
                        palette->property(mode == 0 ? "spectrogramPlot" : "waveformPlot"));
                    QVERIFY(plot != nullptr);

                    QQuickItem *fill = partOf(chosen, QStringLiteral("Checked"));
                    QQuickItem *edge = partOf(chosen, QStringLiteral("CheckedEdge"));
                    QQuickItem *otherIcon = partOf(other, QStringLiteral("Icon"));
                    QVERIFY2(fill && edge && otherIcon, qPrintable(where));
                    const QColor text = plot->property("plotText").value<QColor>();
                    const QColor fillColor = fill->property("color").value<QColor>();
                    QVERIFY2(std::abs(fillColor.alphaF() - 0.18) < 0.01,
                             qPrintable(QStringLiteral("%1: filled at %2").arg(where).arg(fillColor.alphaF())));
                    QCOMPARE(fillColor.rgb(), text.rgb());
                    QCOMPARE(QQmlProperty(edge, QStringLiteral("border.color")).read().value<QColor>().rgba(),
                             colorOf(plot, "segmentEdge"));
                    QCOMPARE(otherIcon->property("color").value<QColor>(), text);
                    QVERIFY2(otherIcon->opacity() == 1.0,
                             qPrintable(QStringLiteral("%1: the other glyph at %2").arg(where).arg(otherIcon->opacity())));

                    // Zoomed: the readout and Reset come first.
                    if (mode == 0) {
                        spectrogram->setProperty("_viewerZoomLevel", 2.0);
                    } else {
                        waveform->setZoomLevel(4.0);
                    }
                    QQuickItem *reset = nullptr;
                    QTRY_VERIFY2((reset = buttonNamed(host, QStringLiteral("Reset Zoom"))) != nullptr
                                     && sceneRectOf(reset).right() < sceneRectOf(buttonNamed(host, QStringLiteral("Spectrogram"))).left(),
                                 qPrintable(where));
                    QQuickItem *close = closeShown
                        ? visibleItemNamed(host, shown.fullscreenMode == 1
                                                     ? QStringLiteral("spectrogramFullscreenCloseButton")
                                                     : QStringLiteral("spectrogramPopupCloseButton"))
                        : nullptr;
                    QVERIFY2(!closeShown || close != nullptr, qPrintable(where));
                    QList<double> places;
                    for (QQuickItem *button : {buttonNamed(host, QStringLiteral("Spectrogram")),
                                               buttonNamed(host, QStringLiteral("Waveform")), close}) {
                        if (button != nullptr) {
                            places.append(host->width() - sceneRectOf(button).left());
                        }
                    }
                    if (!warning) {
                        withoutWarning.insert(where, places);
                    } else {
                        QVERIFY2(places == withoutWarning.value(where), qPrintable(where + QStringLiteral(": moved")));
                        if (closeShown) {
                            QQuickItem *issues = visibleItemNamed(host, QStringLiteral("viewerIssuesButton"));
                            QVERIFY2(issues != nullptr, qPrintable(where));
                            QVERIFY2(sceneRectOf(reset).right() < sceneRectOf(issues).left()
                                         && sceneRectOf(issues).right()
                                             < sceneRectOf(buttonNamed(host, QStringLiteral("Spectrogram"))).left(),
                                     qPrintable(where + QStringLiteral(": the warning is out of order")));
                            // The negative where the plot is as dark as the window, the plot's text otherwise.
                            const bool asTheWindow = (plot->property("plotBg").value<QColor>().lightnessF() < 0.45)
                                == palette->property("themeIsDark").toBool();
                            QCOMPARE(partOf(issues, QStringLiteral("Icon"))->property("color").value<QColor>(),
                                     asTheWindow ? palette->property("negative").value<QColor>() : text);
                        }
                    }
                    if (mode == 0) {
                        spectrogram->setProperty("_viewerZoomLevel", 0.0);
                    } else {
                        waveform->setZoomLevel(1.0);
                    }
                }
            }
            viewer->setProperty("visualizationMode", 0);
            QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
            QTRY_VERIFY(!spectrogram->property("viewerMode").toBool());
        }
    }
}

void TransportInputSmokeTest::fullscreenLaneHeadsAreOnTheStripsTypeScale_data() {
    // 0 in the window, 1 over the whole screen, 2 there without the strip.
    QTest::addColumn<int>("where");
    QTest::newRow("window") << 0;
    QTest::newRow("whole screen") << 1;
    QTest::newRow("whole screen, no strip") << 2;
}

// Over the whole screen the plot's lane heads are on the strip's type scale,
// 1.2 times the window's (they were the window's 12 px under the strip's
// 16 px, two scales on one screen), and under the strip their left edge is
// on its title's, the first lane's label at rest too, so its cross-fade
// between its two places has no sideways step. In the window, and in the
// main window, they keep the window's size and the plates' 5 px inset.
void TransportInputSmokeTest::fullscreenLaneHeadsAreOnTheStripsTypeScale() {
    QFETCH(int, where);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(bridge && viewer && autoHide && layer);
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(palette != nullptr);
    const qreal small = palette->property("smallPointSize").toDouble();
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(spectrogramSurface != nullptr);
    // A track plays, so its lanes have their labels.
    setPlayingTrack(bridge);
    QVERIFY(bridge->setProperty("spectrogramViewMode", 1));
    setChannelCount(bridge, spectrogramSurface, 2);
    autoHide->setProperty("hideDelay", 600000);
    const auto firstHead = [layer]() -> QQuickItem * {
        for (QQuickItem *item : visibleItemsNamed(layer, QStringLiteral("plotLaneHead"))) {
            if (item->property("channelIndex").toInt() == 0) {
                return item;
            }
        }
        return nullptr;
    };
    const auto labelPoints = [](QQuickItem *head) {
        QQuickItem *text = itemNamed(itemNamed(head, QStringLiteral("plotLaneLabel")),
                                     QStringLiteral("plotPlateLabel"));
        return text ? qvariant_cast<QFont>(text->property("font")).pointSizeF() : -1.0;
    };

    // The main window's, before and after.
    QQuickItem *head = nullptr;
    QTRY_VERIFY((head = firstHead()) != nullptr);
    QVERIFY(qAbs(labelPoints(head) - small) < 0.01);
    QCOMPARE(sceneRectOf(head).left() - sceneRectOf(layer).left(), 5.0);

    bridge->setProperty("showFullscreenTitleStrip", where != 2);
    bridge->setProperty("viewerFullscreenMode", where == 0 ? 0 : 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *host = window;
    if (where != 0) {
        host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
        QVERIFY(host != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(host));
    } else {
        QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
        QVERIFY(popup != nullptr);
        QTRY_VERIFY(popup->property("opened").toBool());
    }
    QTRY_COMPARE(layer->window(), host);
    QTRY_VERIFY((head = firstHead()) != nullptr);
    const qreal points = where == 0 ? small : small * 1.2;
    QTRY_VERIFY2(qAbs(labelPoints(head) - points) < 0.01, qPrintable(QString::number(labelPoints(head))));
    // Mute and Solo's letters with it.
    QQuickItem *mute = itemNamed(head, QStringLiteral("plotLaneMuteButton"));
    QVERIFY(mute != nullptr);
    QVERIFY(qAbs(mute->property("glyphPointSize").toDouble() - points) < 0.01);
    if (where == 1) {
        QQuickItem *title = visibleItemNamed(host, QStringLiteral("viewerStripTitle"));
        QVERIFY(title != nullptr);
        QTRY_COMPARE(sceneRectOf(head).left(), sceneRectOf(title).left());
        // The first lane's label at rest, where the strip is hidden, at the
        // same x, on the same scale.
        QQuickItem *rest = itemNamed(head, QStringLiteral("plotLaneRestLabel"));
        QVERIFY(rest != nullptr);
        QCOMPARE(sceneRectOf(rest).left(), sceneRectOf(itemNamed(head, QStringLiteral("plotLaneLabel"))).left());
        QVERIFY(qAbs(qvariant_cast<QFont>(itemNamed(rest, QStringLiteral("plotPlateLabel"))->property("font"))
                         .pointSizeF() - points) < 0.01);
    } else {
        QTRY_COMPARE(sceneRectOf(head).left() - sceneRectOf(layer).left(), 5.0);
    }

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    if (where != 0) {
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
    // Back in the main window, the window's again.
    QTRY_COMPARE(layer->window(), window);
    QTRY_VERIFY((head = firstHead()) != nullptr);
    QTRY_VERIFY(qAbs(labelPoints(head) - small) < 0.01);
    QTRY_COMPARE(sceneRectOf(head).left() - sceneRectOf(layer).left(), 5.0);
}

void TransportInputSmokeTest::fullscreenPlotTextIsOnTheStripsTypeScale_data() {
    // 0 in the window, 1 over the whole screen, 2 there without the strip.
    QTest::addColumn<int>("where");
    QTest::newRow("window") << 0;
    QTest::newRow("whole screen") << 1;
    QTest::newRow("whole screen, no strip") << 2;
}

// Over the whole screen the plots' own text, the crosshair's readout and the
// scale's labels, is on the strip's type scale with the lane heads, 1.2
// times the window's (it was the window's 12 px under the strip's 16 px). In
// the window, and in the main window before and after, it is the window's.
void TransportInputSmokeTest::fullscreenPlotTextIsOnTheStripsTypeScale() {
    QFETCH(int, where);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveform = window->findChild<QQuickItem *>(QStringLiteral("waveformEditorItem"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(bridge && viewer && layer && spectrogramSurface && waveform && palette);
    const qreal fullscreenScale = palette->property("fullscreenTextScale").toDouble();
    QCOMPARE(fullscreenScale, 1.2);
    setPlayingTrack(bridge);
    QVERIFY(bridge->setProperty("spectrogramViewMode", 1));
    setChannelCount(bridge, spectrogramSurface, 2);
    // Every lane's, and the waveform's, and the lane heads' with them.
    const auto checkScale = [&](qreal expected) {
        const QList<QQuickItem *> lanes =
            visibleItemsNamed(spectrogramSurface, QStringLiteral("spectrogramPaneItem"));
        QVERIFY(lanes.size() == 2);
        for (QQuickItem *lane : lanes) {
            QTRY_COMPARE(lane->property("textScale").toDouble(), expected);
        }
        QTRY_COMPARE(waveform->property("textScale").toDouble(), expected);
        QCOMPARE(layer->property("textScale").toDouble(), expected);
    };
    QTRY_COMPARE(visibleItemsNamed(spectrogramSurface, QStringLiteral("spectrogramPaneItem")).size(), 2);
    checkScale(1.0);
    if (QTest::currentTestFailed()) {
        return;
    }

    bridge->setProperty("showFullscreenTitleStrip", where != 2);
    bridge->setProperty("viewerFullscreenMode", where == 0 ? 0 : 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *host = window;
    if (where != 0) {
        host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
        QVERIFY(host != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(host));
    } else {
        QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
        QVERIFY(popup != nullptr);
        QTRY_VERIFY(popup->property("opened").toBool());
    }
    QTRY_COMPARE(layer->window(), host);
    checkScale(where == 0 ? 1.0 : fullscreenScale);
    if (QTest::currentTestFailed()) {
        return;
    }

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    if (where != 0) {
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
    QTRY_COMPARE(layer->window(), window);
    checkScale(1.0);
}

void TransportInputSmokeTest::viewerStripButtonsFollowItsType_data() {
    QTest::addColumn<int>("points");
    QTest::newRow("10 pt") << 10;
    QTest::newRow("24 pt") << 24;
}

// The title strip's buttons follow its type, 1.2 times the window's: 22 px
// glyphs (smallMedium) on 30 px buttons with the default font, larger with
// a larger one, every one of them, the viewer's own as its zoom's and
// Close; with the default font the band stays 38 px and its 1 px edge (at
// the window's 24 px with 16 px glyphs they were a size too small beside
// its title). The windowed viewer's plate keeps the window's 24 px.
void TransportInputSmokeTest::viewerStripButtonsFollowItsType() {
    QFETCH(int, points);
    const ScopedApplicationFont font(points);
    const auto scaled = [points](int size) {
        return qMax(size, int(std::lround(size * qMax(1.0, points / 10.0))));
    };
    const int glyph = scaled(22);
    const int button = glyph + 8;
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
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *art = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge && viewer && spectrogram && art);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    const auto checkButtons = [](QQuickItem *in, int size, int glyphSize) -> QString {
        QList<QQuickItem *> items;
        QList<QQuickItem *> pending{in};
        while (!pending.isEmpty()) {
            QQuickItem *next = pending.takeLast();
            items.append(next);
            pending.append(next->childItems());
        }
        int checked = 0;
        for (QQuickItem *item : std::as_const(items)) {
            QQuickItem *icon = itemNamed(item, QStringLiteral("viewerToolButtonIcon"));
            if (!item->isVisible() || !item->inherits("QQuickAbstractButton") || icon == nullptr
                || icon->parentItem() == nullptr || icon->parentItem()->parentItem() != item) {
                continue;
            }
            ++checked;
            if (item->width() != size || item->height() != size || icon->width() != glyphSize
                || icon->height() != glyphSize) {
                return QStringLiteral("%1 is %2x%3 with a %4 px glyph")
                    .arg(item->objectName()).arg(item->width()).arg(item->height()).arg(icon->width());
            }
        }
        return checked >= 2 ? QString() : QStringLiteral("only %1 buttons").arg(checked);
    };

    // Over the whole screen, zoomed, so the readout and Reset show.
    bridge->setProperty("viewerFullscreenMode", 1);
    for (const bool albumArt : {false, true}) {
        QQuickItem *view = albumArt ? art : spectrogram;
        QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "openAlbumArtViewer" : "openSpectrogramViewer"));
        QTRY_VERIFY(view->window() != nullptr && view->window() != window);
        QQuickWindow *host = view->window();
        QVERIFY(QTest::qWaitForWindowExposed(host));
        if (albumArt) {
            art->setProperty("zoom", 2.0);
        } else {
            QTRY_VERIFY(spectrogram->property("viewerMode").toBool());
            spectrogram->setProperty("_viewerZoomLevel", 2.0);
        }
        QQuickItem *reset = nullptr;
        QTRY_VERIFY((reset = visibleItemNamed(host, QStringLiteral("viewerResetButton"))) != nullptr);
        QQuickItem *strip = visibleItemNamed(host, albumArt ? QStringLiteral("albumArtFullscreenStrip")
                                                            : QStringLiteral("spectrogramFullscreenStrip"));
        QVERIFY(strip != nullptr);
        QQuickItem *readout = visibleItemNamed(host, QStringLiteral("viewerZoomReadout"));
        QVERIFY(readout != nullptr);
        const QString wrong = checkButtons(strip, button, glyph);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        // The readout on the buttons' line.
        QCOMPARE(sceneRectOf(readout).center().y(), sceneRectOf(reset).center().y());
        // The band: 38 px with the default font, and its edge.
        if (points == 10) {
            QCOMPARE(strip->height(), 39.0);
        }
        QVERIFY(strip->height() >= button + 8 + 1);
        QCOMPARE(strip->property("closeAxisInset").toDouble(), 8.0 + button / 2.0);
        if (albumArt) {
            art->setProperty("zoom", 1.0);
        } else {
            spectrogram->setProperty("_viewerZoomLevel", 0.0);
        }
        QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "closeAlbumArtViewer" : "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property(albumArt ? "albumArtViewerOpen" : "spectrogramViewerOpen").toBool());
    }
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));

    // In the window, the plate's buttons are the window's.
    bridge->setProperty("viewerFullscreenMode", 0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickItem *plate = nullptr;
    QTRY_VERIFY((plate = visibleItemNamed(window, QStringLiteral("spectrogramPopupControls"))) != nullptr);
    const int windowButton = qMax(24, scaled(16) + 8);
    const QString wrong = checkButtons(plate, windowButton, scaled(16));
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

void TransportInputSmokeTest::viewerStripIsOnOneTypeScale_data() {
    QTest::addColumn<int>("points");
    QTest::newRow("10 pt") << 10;
    QTest::newRow("24 pt") << 24;
}

// The title strip has one type scale: its zoom readout is at the strip's
// text size, 1.2 times the window's, as the time beside it (it was the
// window's small size, 12 px in a 16 px strip), and the gaps between the
// title, the album and the time follow the font (14 px beside 38 px type,
// the album and the time ran together at 24 pt).
void TransportInputSmokeTest::viewerStripIsOnOneTypeScale() {
    QFETCH(int, points);
    const ScopedApplicationFont font(points);
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
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *art = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge && viewer && spectrogram && art);
    setPlayingTrack(bridge);
    // Short enough that none of them is elided in the test's screen.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Tide"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("Q"));
    bridge->setProperty("currentTrackAlbum", QString());
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", 1);
    for (const bool albumArt : {false, true}) {
        QQuickItem *view = albumArt ? art : spectrogram;
        QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "openAlbumArtViewer" : "openSpectrogramViewer"));
        QTRY_VERIFY(view->window() != nullptr && view->window() != window);
        QQuickWindow *host = view->window();
        QVERIFY(QTest::qWaitForWindowExposed(host));
        if (albumArt) {
            art->setProperty("zoom", 2.0);
        } else {
            QTRY_VERIFY(spectrogram->property("viewerMode").toBool());
            spectrogram->setProperty("_viewerZoomLevel", 2.0);
        }
        QQuickItem *readout = nullptr;
        QTRY_VERIFY((readout = visibleItemNamed(host, QStringLiteral("viewerZoomReadout"))) != nullptr);
        QQuickItem *title = visibleItemNamed(host, QStringLiteral("viewerStripTitle"));
        QQuickItem *context = visibleItemNamed(host, QStringLiteral("viewerStripContext"));
        QQuickItem *time = visibleItemNamed(host, QStringLiteral("viewerStripTime"));
        QVERIFY(title && context && time);
        const qreal size = qvariant_cast<QFont>(time->property("font")).pointSizeF();
        QVERIFY(qAbs(size - points * 1.2) < 0.01);
        const qreal readoutSize = qvariant_cast<QFont>(readout->property("font")).pointSizeF();
        QVERIFY2(qAbs(readoutSize - size) < 0.01, qPrintable(QStringLiteral("the readout at %1").arg(readoutSize)));
        // The gaps at the default zoom: at 24 pt in the test's 800 px screen,
        // the zoom's readout and Reset leave the texts too little room.
        if (albumArt) {
            art->setProperty("zoom", 1.0);
        } else {
            spectrogram->setProperty("_viewerZoomLevel", 0.0);
        }
        QTRY_VERIFY(!readout->isVisible());
        // 14 px at 10 pt, as much more as the font is larger.
        const qreal gap = std::round(14.0 * points / 10.0);
        const auto between = [](QQuickItem *left, QQuickItem *right) {
            return right->mapToScene(QPointF(0, 0)).x() - left->mapToScene(QPointF(left->width(), 0)).x();
        };
        QTRY_VERIFY2(qAbs(between(title, context) - gap) < 0.01 && qAbs(between(context, time) - gap) < 0.01,
                     qPrintable(QStringLiteral("gaps of %1 and %2").arg(between(title, context)).arg(between(context, time))));
        QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "closeAlbumArtViewer" : "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property(albumArt ? "albumArtViewerOpen" : "spectrogramViewerOpen").toBool());
    }
}

void TransportInputSmokeTest::viewerDragsCountAsPointerActivity_data() {
    // 0 the spectrogram, 1 the waveform, 2 the album art.
    QTest::addColumn<int>("view");
    QTest::addColumn<int>("displayMode");
    QTest::addColumn<int>("fullscreenMode");
    for (const int fullscreenMode : {0, 1}) {
        const QByteArray where = fullscreenMode == 1 ? ", whole screen" : ", window";
        QTest::newRow(("spectrogram, rolling" + where).constData()) << 0 << 0 << fullscreenMode;
        QTest::newRow(("spectrogram, centred" + where).constData()) << 0 << 1 << fullscreenMode;
        QTest::newRow(("waveform, rolling" + where).constData()) << 1 << 0 << fullscreenMode;
        QTest::newRow(("waveform, centred" + where).constData()) << 1 << 1 << fullscreenMode;
        QTest::newRow(("album art" + where).constData()) << 2 << 1 << fullscreenMode;
    }
}

// A drag on the plot or the art is pointer activity, for as long as the
// button is down: a pan longer than the hide delay, paused in it or not,
// keeps its hand and its crosshair (the hover that counted as activity
// stops while a button is down, so they left 5 s into a pan). Let go, the
// controls hide after the delay again; a press that moves nothing does not
// hold them.
void TransportInputSmokeTest::viewerDragsCountAsPointerActivity() {
    QFETCH(int, view);
    QFETCH(int, displayMode);
    QFETCH(int, fullscreenMode);
    const bool albumArt = view == 2;
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
    QObject *autoHide = window->findChild<QObject *>(albumArt ? QStringLiteral("albumArtFullscreenControlsAutoHide")
                                                              : QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *art = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    auto *spectrogram = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge && viewer && autoHide && art && spectrogram && waveformSurface);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("spectrogramDisplayMode", displayMode);
    bridge->setProperty("showSpectrogramCrosshair", true);
    bridge->setProperty("viewerFullscreenMode", fullscreenMode);
    if (view == 1) {
        viewer->setProperty("visualizationMode", 1);
        QTRY_VERIFY(waveform->isVisible());
    }
    autoHide->setProperty("hideDelay", 600000);
    QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "openAlbumArtViewer" : "openSpectrogramViewer"));
    QQuickItem *surface = albumArt ? art : (view == 1 ? waveformSurface : spectrogram);
    QQuickWindow *host = window;
    if (fullscreenMode == 1) {
        QTRY_VERIFY(surface->window() != nullptr && surface->window() != window);
        host = surface->window();
        QVERIFY(QTest::qWaitForWindowExposed(host));
    } else {
        QObject *popup = window->findChild<QObject *>(albumArt ? QStringLiteral("albumArtViewerPopup")
                                                               : QStringLiteral("spectrogramViewerPopup"));
        QTRY_VERIFY(popup->property("opened").toBool());
    }
    if (albumArt) {
        QTRY_COMPARE(art->property("imageStatus").toInt(), 1);
        art->setProperty("zoom", 4.0);
    } else {
        QTRY_VERIFY(spectrogram->property("viewerMode").toBool());
        if (view == 1) {
            waveform->setZoomLevel(4.0);
        } else {
            spectrogram->setProperty("_viewerZoomLevel", 4.0);
        }
    }
    const QPoint from = sceneRectOf(surface).center().toPoint();
    QTest::mouseMove(host, from + QPoint(1, 1));
    QTest::mouseMove(host, from);
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    const Qt::MouseButton button = albumArt ? Qt::LeftButton : Qt::MiddleButton;
    constexpr int delay = 120;

    QTest::mousePress(host, button, Qt::NoModifier, from);
    QPoint at = from;
    at.rx() -= 40;
    QTest::mouseMove(host, at);
    autoHide->setProperty("hideDelay", delay);
    const auto kept = [&]() -> QString {
        if (!autoHide->property("controlsVisible").toBool() || autoHide->property("level").toDouble() < 1.0) {
            return QStringLiteral("the controls left");
        }
        if (albumArt || view == 1) {
            // The hand of the pan.
            if (host->cursor().shape() != Qt::ClosedHandCursor) {
                return QStringLiteral("the hand left: %1").arg(int(host->cursor().shape()));
            }
        } else if (host->cursor().shape() == Qt::BlankCursor) {
            return QStringLiteral("the pointer left");
        }
        if (view == 1 && waveform->property("crosshairOpacity").toDouble() < 1.0) {
            return QStringLiteral("the waveform's crosshair left");
        }
        if (view == 0) {
            for (QQuickItem *lane : itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"))) {
                if (lane->property("crosshairOpacity").toDouble() < 1.0) {
                    return QStringLiteral("the spectrogram's crosshair left");
                }
            }
        }
        return {};
    };
    // Dragging for longer than the delay, then resting in the drag as long.
    for (int step = 0; step < 14; ++step) {
        QTest::qWait(delay / 4);
        if (step < 7) {
            at.rx() -= 4;
            QTest::mouseMove(host, at);
        }
        QVERIFY2(kept().isEmpty(), qPrintable(QStringLiteral("step %1: %2").arg(step).arg(kept())));
    }
    QTest::mouseRelease(host, button, Qt::NoModifier, at);
    QVERIFY(autoHide->property("controlsVisible").toBool());
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());

    // A press that moves nothing holds nothing.
    autoHide->setProperty("hideDelay", 600000);
    QTest::mouseMove(host, from + QPoint(7, 3));
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    QTest::mousePress(host, button, Qt::NoModifier, from + QPoint(7, 3));
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTest::mouseRelease(host, button, Qt::NoModifier, from + QPoint(7, 3));

    autoHide->setProperty("hideDelay", 600000);
    QVERIFY(QMetaObject::invokeMethod(viewer, albumArt ? "closeAlbumArtViewer" : "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property(albumArt ? "albumArtViewerOpen" : "spectrogramViewerOpen").toBool());
}

std::unique_ptr<QmlSmokeTest> makeTransportInputSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<TransportInputSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_transport_input.moc"
