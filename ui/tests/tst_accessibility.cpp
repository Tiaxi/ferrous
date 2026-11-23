// SPDX-License-Identifier: GPL-3.0-or-later

// Keyboard reach, accessible names, font scaling and theme colours.

#include <QAccessible>
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QJSValue>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <Kirigami/Platform/Units>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>

#include "../src/BridgeClient.h"
// The plot layer's tests give the meter levels to draw.
#define private public
#include "../src/DiagnosticsLogView.h"
#include "../src/InputModality.h"
#include "../src/LevelMeterItem.h"
#undef private
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/PlaybackClock.h"
#include "../src/SpectrogramItem.h"
#include "../src/StandardKeys.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"
#include "../src/WindowPresence.h"
#include "TestColor.h"
#include "TestItems.h"
#include "TestMainWindow.h"
#include "TestMessages.h"
#include "TestMotion.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

// The controls style that the test program's main() sets, rather than the
// app's, for a child process that asks for another.
constexpr char childControlsStyleVariable[] = "FERROUS_TEST_CHILD_CONTROLS_STYLE";

// A larger font widens the columns and raises the window's minimum size
// with them; the offscreen platform warns that it ignores the new minimum.
void dropMinimumSizeWarnings() {
    dropCapturedMessages(QStringLiteral("This plugin does not support propagateSizeHints()"));
}

QString formatQmlErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    for (const QQmlError &error : errors) {
        lines.push_back(error.toString());
    }
    return lines.join('\n');
}

const QUrl &harnessBaseUrl() {
    static const QUrl url = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/AccessibilityHarness.qml"));
    return url;
}

QObject *createQml(QQmlEngine &engine, const QByteArray &source, QString *errorText) {
    QQmlComponent component(&engine);
    component.setData(source, harnessBaseUrl());
    QObject *object = component.isError() ? nullptr : component.create();
    if (object == nullptr && errorText != nullptr) {
        *errorText = formatQmlErrors(component.errors());
    }
    return object;
}

// The first visible item named `objectName` in `window`.
QQuickItem *visibleItemNamed(QQuickWindow *window, const QString &objectName) {
    for (QQuickItem *item : itemsNamed(window->contentItem(), objectName)) {
        if (item->isVisible()) {
            return item;
        }
    }
    return nullptr;
}

// The visible items named `objectName` inside `root`, top to bottom.
QList<QQuickItem *> visibleRows(QQuickItem *root, const QString &objectName) {
    QList<QQuickItem *> rows;
    for (QQuickItem *item : itemsNamed(root, objectName)) {
        if (item->isVisible() && item->height() > 0) {
            rows.push_back(item);
        }
    }
    std::sort(rows.begin(), rows.end(), [](QQuickItem *a, QQuickItem *b) { return a->y() < b->y(); });
    return rows;
}

QString accessibleText(QObject *object, QAccessible::Text text) {
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(object);
    return iface != nullptr ? iface->text(text) : QStringLiteral("<no interface>");
}

QAccessible::Role accessibleRole(QObject *object) {
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(object);
    return iface != nullptr ? iface->role() : QAccessible::NoRole;
}

// `object`'s interface where assistive technology finds it: in the tree that
// starts at `window`. Null when it is not there.
QAccessibleInterface *inAccessibleTree(QQuickWindow *window, QObject *object) {
    const std::function<QAccessibleInterface *(QAccessibleInterface *)> visit =
        [&](QAccessibleInterface *iface) -> QAccessibleInterface * {
        if (iface == nullptr) {
            return nullptr;
        }
        if (iface->object() == object) {
            return iface;
        }
        for (int i = 0; i < iface->childCount(); ++i) {
            if (QAccessibleInterface *found = visit(iface->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    return visit(QAccessible::queryAccessibleInterface(window));
}

qreal callReal(QObject *object, const char *function) {
    QVariant result;
    if (!QMetaObject::invokeMethod(object, function, Q_RETURN_ARG(QVariant, result))) {
        return -1;
    }
    return result.toReal();
}

bool sendWheel(QQuickWindow *window, QPointF scenePosition, QPoint angleDelta) {
    QWheelEvent event(scenePosition, window->mapToGlobal(scenePosition), QPoint(), angleDelta,
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QSpontaneKeyEvent::setSpontaneous(&event);
    QCoreApplication::sendEvent(window, &event);
    return event.isAccepted();
}

QVariantList sampleQueueRows(int count) {
    QVariantList rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(QVariantMap{
            {QStringLiteral("title"), QStringLiteral("Track %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist %1").arg(i)},
            {QStringLiteral("album"), QStringLiteral("Album")},
            {QStringLiteral("lengthText"), QStringLiteral("1:00")},
        });
    }
    return rows;
}

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8(QByteArray &out, const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    appendLe<quint32>(out, static_cast<quint32>(utf8.size()));
    out.append(utf8);
}

// An artist with an album of one track, in the bridge's tree format.
QByteArray artistAlbumTrackTree() {
    struct Row {
        quint8 rowType;
        quint16 depth;
        quint32 childCount;
        QString title;
        QString key;
        QString trackPath;
    };
    const QString trackPath = QStringLiteral("/music/Artist A/Album A/01.flac");
    const QList<Row> rows{
        {1, 0, 1, QStringLiteral("Artist A"), QStringLiteral("artist|Artist A"), {}},
        {2, 1, 1, QStringLiteral("Album A"), QStringLiteral("album|Artist A|Album A"), {}},
        {4, 2, 0, QStringLiteral("01 - First"), QStringLiteral("track|") + trackPath, trackPath},
    };
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const Row &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, -1);
        appendLe<quint32>(out, 0);
        appendLe<quint32>(out, row.childCount);
        appendLe<qint32>(out, 0);
        appendUtf8(out, row.title);
        appendUtf8(out, row.key);
        appendUtf8(out, QStringLiteral("Artist A"));
        appendUtf8(out, row.trackPath);
        appendUtf8(out, QString());
        appendLe<quint64>(out, 0);
        appendUtf8(out, row.trackPath);
        appendLe<quint32>(out, row.trackPath.isEmpty() ? 0 : 1);
        if (!row.trackPath.isEmpty()) {
            appendUtf8(out, row.trackPath);
        }
    }
    return out;
}

} // namespace

class AccessibilityTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void controlsAreNamedForAssistiveTechnology();
    void albumArtSaysWhatItsFocusStopDoes();
    void slidersAnnounceWholeSecondsAndPercents();
    void arrowKeysStepTheVolumeFromItsValue();
    void bridgeEchoesWaitForTheVolumeGesture();
    void arrowKeysSeekFromThePlaybackPosition();
    void seekDragSeeksOnceOnRelease();
    void heldArrowKeysAccumulateVolumeSteps();
    void heldArrowKeysSeekOnceWhenReleased();
    void overlappingArrowKeysAreOneGesture();
    void seekDragOutlastsAKeyRelease();
    void seekDragOutlastsTheFocus();
    void seekClickSeeksOnce();
    void seekDragShowsAndSeeksTheTimeUnderThePointer();
    void seekClickSeeksToTheTimeUnderThePointer();
    void seekPointerReachesEitherEndOnItsWholeSecond();
    void playbackMovesTheSeekBarByWholePixels();
    void seekBarStepsByTheWindowsDevicePixels();
    void seekBarPlayheadIsOneOutlinedLine();
    void seekBarPlayheadStaysOnTheBarAtEitherEnd();
    void seekHoverShowsTheTimeUnderThePointer();
    void seekHoverShowsTheTimeAClickSeeksTo();
    void seekHoverFollowsTheWindowsDevicePixels();
    void seekGesturesShowTheTimeOverThePlayhead();
    void seekHoverShowsNothingWithoutATrack();
    void seekTimeFadesOutWhereItWas();
    void seekTouchShowsNoHoverTime();
    void seekTouchShowsNoHoverTimeAfterTheMouseMoved();
    void seekTouchShowsNoHoverTimeWhileTheMouseIsInAnotherWindow();
    void seekHoverReturnsWhenTheMouseEnters();
    void seekTouchShowsNoHoverTimeWhenTheMouseScrolls();
    void seekTouchMadeIntoMouseEventsShowsNoHoverTime();
    void seekBarPlayheadContrastsWithTheBar_data();
    void seekBarPlayheadContrastsWithTheBar();
    void playbackClockMovesTheSeekBarByPixelsAndItsTextsBySeconds();
    void seekBarDescriptionFollowsTheSecondsBetweenPixels();
    void seekBarCatchesUpWithRealPlayback();
    void seekBarStepsFromThePositionBetweenItsPixels();
    void transportBarHasNoWarnings();
    void transportBarHasNoWarningsUnderTheBasicStyle();
    void rowHeightsAndScrollingFollowTheFont();
    void queueHeaderAndRowsShareTheColumnWidths();
    void lightPaletteLeavesNoFixedColours();
    void transportLeavesTheKeyboardWhereItWas();
    void queueColumnsResizeWithinTheirLimits();
    void modeSwitchIsTwoRadioButtons();
    void plotLayerShowsThePaneStatesAndTheMeter();
    void laneButtonsAreTogglesTheKeyboardReaches();
    void plotLayerDrawsWithContrast();
    void drawsOffscreenUnlessAPlatformIsChosen();

private:
    // The checks that the tests above run in turn in one window; each says
    // what it checks where it is defined.
    void checkVisualizationsHaveAccessibleRoles(QQuickWindow *window);
    void checkLibraryAndQueueRowsHaveAccessibleNames(MainWindowFixture &main, QueueRowsModel &model);
    void checkSlidersAndIconButtonsHaveNamesAndTooltips(QQuickWindow *window);
    void checkKeyboardMovesOfTheSeekSliderSeek(QQuickWindow *window);
    void checkScrobbleIndicatorIsNamedAndWorksByKeyboard(QQuickWindow *window);
    void checkEveryPreferencesPageIsReachableByTab(QQuickWindow *window);
    void checkClicksLeaveSpaceToPlayOrPause(QQuickWindow *window);
    void checkModifiedKeysReachTheShortcutsFromFocusedButtons(QQuickWindow *window);
    void checkQueueColumnsResizeByTheirHandles(QQuickWindow *window);
    void checkACompressedQueueColumnResizesFromItsPreferredWidth(QQuickWindow *window);
    void checkQueueColumnTextStaysInsideItsColumnAtTheMinimum(QQuickWindow *window);
    void checkModeSwitchSegmentsAreCheckableRadioButtons(QQuickWindow *window);
    void checkModeSwitchIconFollowsTheMode(QQuickWindow *window);
    void checkModeSwitchStatesStayApart(QQuickWindow *window, int mode);
    void checkPlotLayerShowsThePaneStates(QQuickWindow *window);
    void checkLevelMeterReservesItsColumnAndReadsOut(QQuickWindow *window);
    void checkLaneButtonsAreTabStopsThatRevealThemselves(QQuickWindow *window);
    void checkLaneButtonsAreCheckableToggles(QQuickWindow *window);
};

void AccessibilityTest::initTestCase() {
    startCapturingMessages();
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<SpectrogramViewFollow>("FerrousUi", 1, 0, "SpectrogramViewFollow");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<DiagnosticsLogView>("FerrousUi", 1, 0, "DiagnosticsLogView");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
    InputModality::registerQmlTypes();
    StandardKeys::registerQmlTypes();
}

void AccessibilityTest::init() {
    takeCapturedMessagesText();
}

void AccessibilityTest::cleanup() {
    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
}

void AccessibilityTest::cleanupTestCase() {
    stopCapturingMessages();
}

// The main window's controls, lists and plots tell assistive technology
// what they are, and the keyboard reaches them.
void AccessibilityTest::controlsAreNamedForAssistiveTechnology() {
    // Outlives the window that shows it.
    QueueRowsModel model;
    MainWindowFixture main;
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkVisualizationsHaveAccessibleRoles(window); },
               [&] { checkLibraryAndQueueRowsHaveAccessibleNames(main, model); },
               [&] { checkSlidersAndIconButtonsHaveNamesAndTooltips(window); },
               [&] { checkKeyboardMovesOfTheSeekSliderSeek(window); },
               [&] { checkScrobbleIndicatorIsNamedAndWorksByKeyboard(window); },
               [&] { checkEveryPreferencesPageIsReachableByTab(window); }});
}

// The album art's focus stop tells assistive technology what it does, and
// does it when pressed (#732): a cover is a button that opens the album art
// viewer, as a double-click does, and a track without one has a button that
// opens its menu, as Menu and Shift+F10 do; each names those keys.
void AccessibilityTest::albumArtSaysWhatItsFocusStopDoes() {
    MainWindowFixture main;
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    QVERIFY(bridge->setProperty("currentTrackTitle", QStringLiteral("Shore")));
    QVERIFY(bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/track.flac")));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Playing")));
    QQuickItem *stop = nullptr;
    QTRY_VERIFY((stop = visibleItemNamed(window, QStringLiteral("albumArtFocusTarget"))) != nullptr);
    QTRY_VERIFY(stop->isEnabled());

    // No album art: a button with a menu.
    QAccessibleInterface *iface = inAccessibleTree(window, stop);
    QVERIFY(iface != nullptr);
    QCOMPARE(iface->role(), QAccessible::ButtonMenu);
    QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("No album art"));
    QCOMPARE(iface->text(QAccessible::Description),
             QStringLiteral("Opens a menu of album art actions. Press Menu or Shift+F10 to open it."));
    QVERIFY(iface->state().focusable);
    QAccessibleActionInterface *actions = iface->actionInterface();
    QVERIFY(actions != nullptr);
    QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
    const auto openMenu = [&]() -> QObject * {
        for (QObject *menu : window->findChildren<QObject *>(QStringLiteral("albumArtMenu"))) {
            if (menu->property("opened").toBool()) {
                return menu;
            }
        }
        return nullptr;
    };
    actions->doAction(QAccessibleActionInterface::pressAction());
    QObject *menu = nullptr;
    QTRY_VERIFY((menu = openMenu()) != nullptr);
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    QTRY_VERIFY(openMenu() == nullptr);
    // Enter presses it too, as the role promises.
    stop->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(stop->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY((menu = openMenu()) != nullptr);
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    QTRY_VERIFY(openMenu() == nullptr);

    // A cover: a button that opens the viewer, its menu on the keys.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage cover(64, 64, QImage::Format_RGB32);
    cover.fill(QColor(200, 120, 40));
    const QString coverPath = directory.filePath(QStringLiteral("cover.png"));
    QVERIFY(cover.save(coverPath));
    QVERIFY(bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString()));
    QTRY_COMPARE(iface->role(), QAccessible::Button);
    QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Album art"));
    QCOMPARE(iface->text(QAccessible::Description),
             QStringLiteral("Opens the album art viewer. Press Menu or Shift+F10 for more actions."));
    actions = iface->actionInterface();
    QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
    actions->doAction(QAccessibleActionInterface::pressAction());
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(openMenu() == nullptr);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY(window->isActive());
    stop->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(stop->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Enter, Qt::KeypadModifier);
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

// What the transport's clicks and keys leave the keyboard on.
void AccessibilityTest::transportLeavesTheKeyboardWhereItWas() {
    MainWindowFixture main;
    QVERIFY2(main.window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkClicksLeaveSpaceToPlayOrPause(main.window); },
               [&] { checkModifiedKeysReachTheShortcutsFromFocusedButtons(main.window); }});
}

// The queue's columns, resized by their handles and their saved widths.
// Each check sets the widths it starts from.
void AccessibilityTest::queueColumnsResizeWithinTheirLimits() {
    MainWindowFixture main;
    QVERIFY2(main.window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkQueueColumnsResizeByTheirHandles(main.window); },
               [&] { checkACompressedQueueColumnResizesFromItsPreferredWidth(main.window); },
               [&] { checkQueueColumnTextStaysInsideItsColumnAtTheMinimum(main.window); }});
}

// The plot's mode switch, read and used as a pair of radio buttons, on
// either plot.
void AccessibilityTest::modeSwitchIsTwoRadioButtons() {
    MainWindowFixture main;
    QVERIFY2(main.window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkModeSwitchSegmentsAreCheckableRadioButtons(main.window); },
               [&] { checkModeSwitchIconFollowsTheMode(main.window); },
               [&] { checkModeSwitchStatesStayApart(main.window, 0); },
               [&] { checkModeSwitchStatesStayApart(main.window, 1); }});
}

// The plot layer's messages before and during a track, and its level
// meter's column. The pane's states come first, before any track.
void AccessibilityTest::plotLayerShowsThePaneStatesAndTheMeter() {
    MainWindowFixture main;
    QVERIFY2(main.window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkPlotLayerShowsThePaneStates(main.window); },
               [&] { checkLevelMeterReservesItsColumnAndReadsOut(main.window); }});
}

// The lanes' Mute and Solo, from the keyboard and to assistive technology.
// Before playback, as the Tab order reaches them from the mode switch,
// which hides once playback has run a while.
void AccessibilityTest::laneButtonsAreTogglesTheKeyboardReaches() {
    MainWindowFixture main;
    QVERIFY2(main.window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkLaneButtonsAreTabStopsThatRevealThemselves(main.window); },
               [&] { checkLaneButtonsAreCheckableToggles(main.window); }});
}

void AccessibilityTest::checkEveryPreferencesPageIsReachableByTab(QQuickWindow *window) {
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("preferencesDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    // Configure Ferrous has a window of its own, which takes the keys.
    auto *prefs = qobject_cast<QQuickWindow *>(dialog);
    QVERIFY(prefs != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(prefs));
    QVERIFY(QTest::qWaitForWindowActive(prefs));
    QObject *pages = nullptr;
    QTRY_VERIFY((pages = visibleItemNamed(prefs, QStringLiteral("preferencesPages"))) != nullptr);
    // The page list is a list of page tabs, and has the keyboard first.
    QQuickItem *pageList = visibleItemNamed(prefs, QStringLiteral("preferencesPageList"));
    QVERIFY(pageList != nullptr);
    QCOMPARE(accessibleRole(pageList), QAccessible::PageTabList);
    QTRY_VERIFY(pageList->hasActiveFocus());
    const int pageCount = 4;
    for (int i = 0; i < pageCount; ++i) {
        QQuickItem *page = nullptr;
        QTRY_VERIFY((page = visibleItemNamed(prefs, QStringLiteral("preferencesPage_%1").arg(i))) != nullptr);
        QCOMPARE(accessibleRole(page), QAccessible::PageTab);
    }
    QCOMPARE(pages->property("currentIndex").toInt(), 0);

    // The arrow keys move down the list, and Ctrl+Tab cycles the pages.
    const auto tabTo = [&](QQuickItem *target) {
        for (int presses = 0; presses < 60 && !target->hasActiveFocus(); ++presses) {
            QTest::keyClick(prefs, Qt::Key_Tab);
        }
        return target->hasActiveFocus();
    };
    for (int i = 1; i < pageCount; ++i) {
        QTest::keyClick(prefs, Qt::Key_Down);
        QTRY_COMPARE(pages->property("currentIndex").toInt(), i);
    }
    QTest::keyClick(prefs, Qt::Key_Tab, Qt::ControlModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 0);
    QTest::keyClick(prefs, Qt::Key_PageUp, Qt::ControlModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), pageCount - 1);
    QTest::keyClick(prefs, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), pageCount - 2);

    // The check boxes on the pages take focus too, and Space toggles them;
    // OK applies the change.
    QTest::keyClick(prefs, Qt::Key_PageUp, Qt::ControlModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 1);
    QQuickItem *levelMeterBox = visibleItemNamed(prefs, QStringLiteral("showLevelMeterCheckBox"));
    QVERIFY(levelMeterBox != nullptr);
    // Off by default for a new install.
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(!bridge->property("showLevelMeter").toBool());
    QVERIFY(!levelMeterBox->property("checked").toBool());
    QVERIFY(tabTo(levelMeterBox));
    QTest::keyClick(prefs, Qt::Key_Space);
    QTRY_VERIFY(levelMeterBox->property("checked").toBool());
    // Nothing changes until OK.
    QVERIFY(!bridge->property("showLevelMeter").toBool());
    QQuickItem *ok = visibleItemNamed(prefs, QStringLiteral("preferencesOkButton"));
    QVERIFY(ok != nullptr);
    QVERIFY(tabTo(ok));
    QTest::keyClick(prefs, Qt::Key_Space);
    QTRY_VERIFY(!prefs->isVisible());
    QVERIFY(bridge->property("showLevelMeter").toBool());
}

// `model` outlives the window that shows it.
void AccessibilityTest::checkLibraryAndQueueRowsHaveAccessibleNames(MainWindowFixture &main, QueueRowsModel &model) {
    QQuickWindow *window = main.window;
    LibraryTreeModel &libraryModel = main.libraryModel;
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    bridge->setProperty("playingQueueIndex", 1);
    bridge->setProperty("playbackState", QStringLiteral("Paused"));

    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(queue != nullptr);
    QCOMPARE(accessibleRole(queue), QAccessible::List);
    QCOMPARE(accessibleText(queue, QAccessible::Name), QStringLiteral("Queue"));
    QList<QQuickItem *> queueRows;
    QTRY_COMPARE((queueRows = visibleRows(queue, QStringLiteral("queueRow"))).size(), 3);
    QCOMPARE(accessibleRole(queueRows[0]), QAccessible::ListItem);
    QCOMPARE(accessibleText(queueRows[0], QAccessible::Name),
             QStringLiteral("1., Track 0, Artist 0, Album, 1:00"));
    QCOMPARE(accessibleText(queueRows[1], QAccessible::Name),
             QStringLiteral("2., Track 1, Artist 1, Album, 1:00, paused"));

    // The bridge's own model gives the rows as roles.
    QueueRowData first;
    first.title = QStringLiteral("Role Title");
    first.artist = QStringLiteral("Role Artist");
    first.album = QStringLiteral("Role Album");
    first.lengthText = QStringLiteral("2:00");
    model.setRows({first});
    bridge->setProperty("playingQueueIndex", -1);
    bridge->setProperty("queueLength", 1);
    bridge->setProperty("queueRows", QVariant::fromValue<QObject *>(&model));
    QTRY_COMPARE((queueRows = visibleRows(queue, QStringLiteral("queueRow"))).size(), 1);
    QCOMPARE(accessibleText(queueRows[0], QAccessible::Name),
             QStringLiteral("1., Role Title, Role Artist, Role Album, 2:00"));

    libraryModel.setExpandedKeys(
        QStringList{QStringLiteral("artist|Artist A"), QStringLiteral("album|Artist A|Album A")});
    libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree());
    QTRY_COMPARE(libraryModel.rowCount(), 3);
    auto *library = window->findChild<QQuickItem *>(QStringLiteral("libraryView"));
    QVERIFY(library != nullptr);
    QCOMPARE(accessibleRole(library), QAccessible::Tree);
    QCOMPARE(accessibleText(library, QAccessible::Name), QStringLiteral("Library"));
    QList<QQuickItem *> libraryRows;
    QTRY_COMPARE((libraryRows = visibleRows(library, QStringLiteral("libraryRow"))).size(), 3);
    const QStringList names{
        QStringLiteral("Artist A"), QStringLiteral("Album A"), QStringLiteral("01 - First")};
    const QStringList kinds{
        QStringLiteral("Artist, expanded"), QStringLiteral("Album, expanded"), QStringLiteral("Track")};
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(accessibleRole(libraryRows[i]), QAccessible::TreeItem);
        QCOMPARE(accessibleText(libraryRows[i], QAccessible::Name), names[i]);
        QCOMPARE(accessibleText(libraryRows[i], QAccessible::Description), kinds[i]);
    }
    QVERIFY(QAccessible::queryAccessibleInterface(libraryRows[0])->state().selectable);
}

void AccessibilityTest::checkScrobbleIndicatorIsNamedAndWorksByKeyboard(QQuickWindow *window) {
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("lastFmScrobblingEnabled", true);
    bridge->setProperty("lastFmAuthState", 2);
    bridge->setProperty("lastFmScrobbleState", 1);
    bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    bridge->setProperty("lastFmScrobbleListenedMs", 30000);

    QQuickItem *indicator = nullptr;
    QTRY_VERIFY((indicator = visibleItemNamed(window, QStringLiteral("scrobbleIndicator"))) != nullptr);
    QCOMPARE(accessibleRole(indicator), QAccessible::Button);
    QCOMPARE(accessibleText(indicator, QAccessible::Name),
             QStringLiteral("Last.fm: 1:30 to scrobble"));
    QCOMPARE(accessibleText(indicator, QAccessible::Description),
             QStringLiteral("Press to pause scrobbling."));
    // Tab reaches it, its tooltip tells the listening left, and
    // Space pauses scrobbling rather than playback. The tooltip without
    // Kirigami's delay: the check is whether it shows, not how soon.
    auto *units = qobject_cast<Kirigami::Platform::Units *>(
        evaluateOn(indicator, QStringLiteral("Kirigami.Units")).value<QObject *>());
    QVERIFY(units != nullptr);
    units->setToolTipDelay(0);
    QVERIFY(indicator->activeFocusOnTab());
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QVERIFY(playPause != nullptr);
    QSignalSpy playPauseTriggered(playPause, SIGNAL(triggered(QObject*)));
    indicator->forceActiveFocus(Qt::TabFocusReason);
    QTRY_COMPARE(evaluateOn(indicator, QStringLiteral("ToolTip.text")).toString(),
                 QStringLiteral("Last.fm: 1:30 to scrobble"));
    QTRY_VERIFY(evaluateOn(indicator, QStringLiteral("ToolTip.visible")).toBool());
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(bridge->property("lastFmScrobblingPaused").toBool());
    QCOMPARE(playPauseTriggered.count(), 0);
    QTRY_COMPARE(accessibleText(indicator, QAccessible::Name),
                 QStringLiteral("Last.fm: scrobbling is paused"));
    QCOMPARE(accessibleText(indicator, QAccessible::Description),
             QStringLiteral("Press to resume scrobbling."));
}

void AccessibilityTest::checkSlidersAndIconButtonsHaveNamesAndTooltips(QQuickWindow *window) {
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("durationSeconds", 200.0);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("volume", 0.5);

    const QList<QPair<QString, QString>> buttons{
        {QStringLiteral("transportButton_previous"), QStringLiteral("Previous")},
        // One button, which shows the state: Pause while playing.
        {QStringLiteral("transportButton_playPause"), QStringLiteral("Pause")},
        {QStringLiteral("transportButton_stop"), QStringLiteral("Stop")},
        {QStringLiteral("transportButton_next"), QStringLiteral("Next")},
        {QStringLiteral("muteButton"), QStringLiteral("Mute")},
    };
    for (const auto &[objectName, name] : buttons) {
        QQuickItem *button = visibleItemNamed(window, objectName);
        QVERIFY2(button != nullptr, qPrintable(objectName));
        QCOMPARE(accessibleRole(button), QAccessible::Button);
        QCOMPARE(accessibleText(button, QAccessible::Name), name);
        QVERIFY2(evaluateOn(button, QStringLiteral("ToolTip.text")).toString().startsWith(name),
                 qPrintable(objectName));
    }
    // The tooltips name the shortcuts too.
    QCOMPARE(evaluateOn(visibleItemNamed(window, QStringLiteral("transportButton_stop")),
                        QStringLiteral("ToolTip.text")).toString(),
             QStringLiteral("Stop · Ctrl+Shift+Space"));

    // Tab reaches the buttons, and Space presses the focused one instead
    // of playing or pausing.
    QQuickItem *next = visibleItemNamed(window, QStringLiteral("transportButton_next"));
    QVERIFY(next->activeFocusOnTab());
    QObject *nextAction = qvariant_cast<QObject *>(next->property("action"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QVERIFY(nextAction != nullptr && playPause != nullptr);
    QSignalSpy nextTriggered(nextAction, SIGNAL(triggered(QObject*)));
    QSignalSpy playPauseTriggered(playPause, SIGNAL(triggered(QObject*)));
    next->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(nextTriggered.count(), 1);
    QCOMPARE(playPauseTriggered.count(), 0);

    QQuickItem *mute = visibleItemNamed(window, QStringLiteral("muteButton"));
    QVERIFY(mute->activeFocusOnTab());
    mute->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(accessibleText(mute, QAccessible::Name), QStringLiteral("Unmute"));
    QCOMPARE(playPauseTriggered.count(), 0);
    QVERIFY(QMetaObject::invokeMethod(mute, "clicked"));
    QTRY_COMPARE(accessibleText(mute, QAccessible::Name), QStringLiteral("Mute"));
    QVERIFY(QMetaObject::invokeMethod(mute, "clicked"));
    QTRY_COMPARE(accessibleText(mute, QAccessible::Name), QStringLiteral("Unmute"));
    QCOMPARE(evaluateOn(mute, QStringLiteral("ToolTip.text")).toString(), QStringLiteral("Unmute"));
    QVERIFY(QMetaObject::invokeMethod(mute, "clicked"));
    QTRY_COMPARE(accessibleText(mute, QAccessible::Name), QStringLiteral("Mute"));

    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(seek != nullptr && volume != nullptr);
    QCOMPARE(accessibleRole(seek), QAccessible::Slider);
    QCOMPARE(accessibleText(seek, QAccessible::Name), QStringLiteral("Seek"));
    QCOMPARE(accessibleText(seek, QAccessible::Description), QStringLiteral("0:00 of 3:20"));
    QAccessibleValueInterface *seekValue = QAccessible::queryAccessibleInterface(seek)->valueInterface();
    QVERIFY(seekValue != nullptr);
    QCOMPARE(seekValue->maximumValue().toDouble(), 200.0);
    QCOMPARE(accessibleRole(volume), QAccessible::Slider);
    QCOMPARE(accessibleText(volume, QAccessible::Name), QStringLiteral("Volume"));
    QTRY_COMPARE(accessibleText(volume, QAccessible::Description), QStringLiteral("50%"));
    QAccessibleValueInterface *volumeValue = QAccessible::queryAccessibleInterface(volume)->valueInterface();
    QVERIFY(volumeValue != nullptr);
    QCOMPARE(volumeValue->currentValue().toDouble(), 50.0);
}

void AccessibilityTest::checkKeyboardMovesOfTheSeekSliderSeek(QQuickWindow *window) {
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("durationSeconds", 200.0);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    QTRY_VERIFY(seek->isEnabled());
    seek->forceActiveFocus(Qt::TabFocusReason);
    QVERIFY(seek->hasActiveFocus());
    // The arrow keys seek by the keyboard's seek step. The fallback bridge
    // has no playback clock, so arrowKeysSeekFromThePlaybackPosition covers
    // steps from a moving position.
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE(bridge->property("lastSeekSeconds").toDouble(), 5.0);
}

namespace {

// The transport bar's controls in the main window, and its actions' spies.
struct TransportFixture {
    QQuickItem *next = nullptr;
    QQuickItem *mute = nullptr;
    QQuickItem *seek = nullptr;
    QQuickItem *volume = nullptr;
    QQuickItem *modeButton = nullptr;
    QQuickItem *library = nullptr;
    QObject *bridge = nullptr;
    QObject *viewer = nullptr;
    std::unique_ptr<QSignalSpy> nextTriggered;
    std::unique_ptr<QSignalSpy> playPauseTriggered;
    std::unique_ptr<QSignalSpy> stopTriggered;

    bool load(QQuickWindow *window) {
        bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        bridge->setProperty("durationSeconds", 200.0);
        bridge->setProperty("playbackState", QStringLiteral("Playing"));
        next = visibleItemNamed(window, QStringLiteral("transportButton_next"));
        mute = visibleItemNamed(window, QStringLiteral("muteButton"));
        seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
        volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
        modeButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
        viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        library = qvariant_cast<QQuickItem *>(window->property("libraryViewRef"));
        if (!next || !mute || !seek || !volume || !modeButton || !viewer || !library) {
            return false;
        }
        QObject *nextAction = qvariant_cast<QObject *>(next->property("action"));
        QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
        QObject *stop = window->findChild<QObject *>(QStringLiteral("stopAction"));
        if (!nextAction || !playPause || !stop) {
            return false;
        }
        nextTriggered = std::make_unique<QSignalSpy>(nextAction, SIGNAL(triggered(QObject*)));
        playPauseTriggered = std::make_unique<QSignalSpy>(playPause, SIGNAL(triggered(QObject*)));
        stopTriggered = std::make_unique<QSignalSpy>(stop, SIGNAL(triggered(QObject*)));
        library->forceActiveFocus();
        return library->hasActiveFocus();
    }
};

QPoint centerOf(QQuickItem *item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

// Presses Tab until `target` has the focus; returns whether it got it.
bool tabTo(QQuickWindow *window, QQuickItem *target) {
    for (int presses = 0; presses < 100 && !target->hasActiveFocus(); ++presses) {
        QTest::keyClick(window, Qt::Key_Tab);
    }
    return target->hasActiveFocus();
}

} // namespace

// The transport bar alone. The harness records what the bar sends. Its
// bridge is updated by the test, as the real bridge's echo comes later, or
// with `echoLags` by the bar's sends, one new value behind.
static QByteArray transportHarnessQml() {
    return QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "panes" as Panes

ApplicationWindow {
    id: harness
    width: 1000
    height: 200
    visible: true
    property alias bridge: bridge
    property alias startButton: startButton
    property real positionSeconds: 50
    // Whole seconds, as the playback clock's text.
    property string positionText: String(Math.floor(positionSeconds))
    // What the transport bar sent, in order.
    property var seeks: []
    property var volumes: []
    property bool echoLags: false

    Components.UiPalette {
        id: uiPalette
        windowRoot: harness
    }

    QtObject {
        id: bridge
        property real durationSeconds: 200
        property string durationText: "03:20"
        property string playbackState: "Playing"
        property real volume: 0.5
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 0
        property bool waveformComplete: false
    }

    Action { id: previousAction; text: "Previous" }
    Action { id: playAction; text: "Play" }
    Action { id: pauseAction; text: "Pause" }
    Action { id: stopAction; text: "Stop" }
    Action { id: nextAction; text: "Next" }

    Column {
        anchors.fill: parent

        Button {
            id: startButton
            text: "Start"
        }

        Panes.TransportBar {
            width: parent.width
            uiBridge: bridge
            uiPalette: uiPalette
            previousAction: previousAction
            playAction: playAction
            pauseAction: pauseAction
            stopAction: stopAction
            nextAction: nextAction
            volumeMuted: false
            keyboardSeekStepSeconds: 5
            displayedPositionSeconds: harness.positionSeconds
            positionText: harness.positionText
            toggleMutedVolume: function() {}
            setAppVolume: function(value) {
                const last = harness.volumes.length > 0
                    ? harness.volumes[harness.volumes.length - 1]
                    : bridge.volume
                // A repeated value brings no newer echo.
                if (harness.echoLags && value !== last) {
                    bridge.volume = last
                }
                harness.volumes = harness.volumes.concat([value])
            }
            normalizedVolumeValue: function(value) { return value }
            seekCommitted: function(value) {
                if (harness.echoLags && harness.seeks.length > 0) {
                    harness.positionSeconds = harness.seeks[harness.seeks.length - 1]
                }
                harness.seeks = harness.seeks.concat([value])
            }
        }
    }
}
)QML");
}

namespace {

QList<double> recordedValues(QObject *harness, const char *name) {
    QList<double> values;
    for (const QVariant &value : qvariant_cast<QJSValue>(harness->property(name)).toVariant().toList()) {
        values.push_back(value.toDouble());
    }
    return values;
}

bool fuzzyEqual(double a, double b) {
    return std::abs(a - b) < 1e-6;
}

// Whether the harness recorded values under `name` after the first `from`,
// and all of those are `expected`.
bool onlyRecordedSince(QObject *harness, const char *name, qsizetype from, double expected) {
    const QList<double> values = recordedValues(harness, name).mid(from);
    return !values.isEmpty()
        && std::all_of(values.begin(), values.end(),
                       [expected](double value) { return fuzzyEqual(value, expected); });
}

bool sameValues(const QList<double> &actual, const QList<double> &expected) {
    return actual.size() == expected.size()
        && std::equal(actual.begin(), actual.end(), expected.begin(), fuzzyEqual);
}

// Sends a key event as the window system does, marked as autorepeat or not.
void sendKey(QQuickWindow *window, QEvent::Type type, Qt::Key key, bool autoRepeat) {
    QKeyEvent event(type, key, Qt::NoModifier, QString(), autoRepeat);
    QSpontaneKeyEvent::setSpontaneous(&event);
    QCoreApplication::sendEvent(window, &event);
}

// Holds `key` down for `steps` steps as Qt sends autorepeat: a press,
// autorepeat release and press pairs, and the final release.
void holdKey(QQuickWindow *window, Qt::Key key, int steps) {
    sendKey(window, QEvent::KeyPress, key, false);
    for (int step = 1; step < steps; ++step) {
        sendKey(window, QEvent::KeyRelease, key, true);
        sendKey(window, QEvent::KeyPress, key, true);
    }
    sendKey(window, QEvent::KeyRelease, key, false);
}

// Holds Left for three steps, steps once with Right while it is held, then
// steps Left once more by autorepeat before releasing it.
void overlapArrowKeys(QQuickWindow *window) {
    sendKey(window, QEvent::KeyPress, Qt::Key_Left, false);
    for (int step = 1; step < 3; ++step) {
        sendKey(window, QEvent::KeyRelease, Qt::Key_Left, true);
        sendKey(window, QEvent::KeyPress, Qt::Key_Left, true);
    }
    sendKey(window, QEvent::KeyPress, Qt::Key_Right, false);
    sendKey(window, QEvent::KeyRelease, Qt::Key_Right, false);
    sendKey(window, QEvent::KeyRelease, Qt::Key_Left, true);
    sendKey(window, QEvent::KeyPress, Qt::Key_Left, true);
    sendKey(window, QEvent::KeyRelease, Qt::Key_Left, false);
}

QString describeValues(const QList<double> &values) {
    QStringList parts;
    for (double value : values) {
        parts.push_back(QString::number(value));
    }
    return QStringLiteral("[") + parts.join(QStringLiteral(", ")) + QStringLiteral("]");
}

// The transport harness with the position of `testClock`, a PlaybackClock
// in the engine's root context, which moves by the seek bar's pixels.
QByteArray clockTransportHarnessQml() {
    QByteArray source = transportHarnessQml();
    const auto replace = [&source](const char *from, const char *to) {
        if (source.count(from) != 1) {
            qFatal("transport harness: no single \"%s\"", from);
        }
        source.replace(from, to);
    };
    replace("displayedPositionSeconds: harness.positionSeconds",
            "displayedPositionSeconds: testClock.positionSeconds\n"
            "            currentPositionSeconds: function() { return testClock.currentPositionSeconds() }");
    replace("positionText: harness.positionText",
            "positionText: testClock.positionText\n"
            "            positionSecond: testClock.positionSecond");
    replace("        Panes.TransportBar {\n", "        Panes.TransportBar {\n            id: transportBar\n");
    replace("    Components.UiPalette {",
            "    Binding {\n"
            "        target: testClock\n"
            "        property: \"positionStepSeconds\"\n"
            "        value: transportBar.positionStepSeconds\n"
            "    }\n\n"
            "    Components.UiPalette {");
    return source;
}

// Loads the transport harness with the focus on its start button.
QQuickWindow *loadTransportHarness(QQmlEngine &engine, QScopedPointer<QObject> &root, QString *errorText,
                                   const QByteArray &source = transportHarnessQml()) {
    root.reset(createQml(engine, source, errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    window->requestActivate();
    if (!QTest::qWaitForWindowActive(window)) {
        return nullptr;
    }
    auto *start = qvariant_cast<QQuickItem *>(root->property("startButton"));
    start->forceActiveFocus(Qt::TabFocusReason);
    return start->hasActiveFocus() ? window : nullptr;
}

// The transport harness of one test. Its window is null, and `error` says
// why, when it did not show.
struct TransportHarness {
    // What has the focus: the start button, or the seek slider, with the
    // bridge's echo one value behind.
    enum Focus { StartButton, SeekWithLaggingEcho };

    explicit TransportHarness(Focus focus = StartButton)
        : window(loadTransportHarness(engine, root, &error)) {
        if (window != nullptr && focus == SeekWithLaggingEcho) {
            root->setProperty("echoLags", true);
            if (!tabTo(window, visibleItemNamed(window, QStringLiteral("seekSlider")))) {
                window = nullptr;
                error = QStringLiteral("the seek slider took no focus");
            }
        }
    }

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString error;
    QQuickWindow *window = nullptr;
};

} // namespace

void AccessibilityTest::slidersAnnounceWholeSecondsAndPercents() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(bridge != nullptr && seek != nullptr && volume != nullptr);
    // What a screen reader reads: the value interface's numbers, of which
    // Orca speaks every decimal, and the description.
    const auto announced = [](QQuickItem *slider) {
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(slider);
        QAccessibleValueInterface *value = iface != nullptr ? iface->valueInterface() : nullptr;
        if (value == nullptr) {
            return QStringLiteral("no value");
        }
        return QStringLiteral("%1 (%2 to %3), %4")
            .arg(value->currentValue().toString(), value->minimumValue().toString(),
                 value->maximumValue().toString(), iface->text(QAccessible::Description));
    };
    const auto seeks = [&root]() { return recordedValues(root, "seeks"); };

    // Playback between two seconds: the second the description gives,
    // while the bar shows the exact position.
    root->setProperty("positionSeconds", 30.353452042);
    QCOMPARE(announced(seek), QStringLiteral("30 (0 to 200), 0:30 of 3:20"));
    QVERIFY(fuzzyEqual(seek->property("shownSeconds").toDouble(), 30.353452042));
    root->setProperty("positionSeconds", 30.6);
    QCOMPARE(announced(seek), QStringLiteral("31 (0 to 200), 0:31 of 3:20"));

    // A key's step, from the exact position, announces the second it
    // reaches and seeks to the exact time.
    QVERIFY(tabTo(window, seek));
    QTest::keyPress(window, Qt::Key_Right);
    QCOMPARE(announced(seek), QStringLiteral("35 (0 to 200), 0:35 of 3:20"));
    QVERIFY(fuzzyEqual(seek->property("shownSeconds").toDouble(), 35.6));
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY2(sameValues(seeks(), {35.6}), qPrintable(describeValues(seeks())));
    root->setProperty("positionSeconds", 35.6);
    QCOMPARE(announced(seek), QStringLiteral("36 (0 to 200), 0:36 of 3:20"));

    // So does the wheel's, and playback then moves the bar on.
    seek->setProperty("wheelEnabled", true);
    QVERIFY(sendWheel(window, seek->mapToScene(QPointF(seek->width() / 2, seek->height() / 2)),
                      QPoint(0, 120)));
    QVERIFY2(sameValues(seeks(), {35.6, 40.6}), qPrintable(describeValues(seeks())));
    root->setProperty("positionSeconds", 40.6);
    QCOMPARE(announced(seek), QStringLiteral("41 (0 to 200), 0:41 of 3:20"));
    root->setProperty("positionSeconds", 42.2);
    QCOMPARE(announced(seek), QStringLiteral("42 (0 to 200), 0:42 of 3:20"));
    QVERIFY(fuzzyEqual(seek->property("shownSeconds").toDouble(), 42.2));

    // A key's step that the slider's whole second takes to the start is
    // still a whole step from the exact time.
    root->setProperty("positionSeconds", 5.3);
    QTest::keyClick(window, Qt::Key_Left);
    QVERIFY2(sameValues(seeks(), {35.6, 40.6, 0.3}), qPrintable(describeValues(seeks())));

    // Within half a second of the start the value is already there, and a
    // step back still seeks to it; likewise at the end.
    root->setProperty("positionSeconds", 0.3);
    QCOMPARE(announced(seek), QStringLiteral("0 (0 to 200), 0:00 of 3:20"));
    QTest::keyClick(window, Qt::Key_Left);
    QVERIFY2(sameValues(seeks(), {35.6, 40.6, 0.3, 0.0}), qPrintable(describeValues(seeks())));
    root->setProperty("positionSeconds", 199.7);
    QCOMPARE(announced(seek), QStringLiteral("200 (0 to 200), 3:20 of 3:20"));
    QTest::keyClick(window, Qt::Key_Right);
    QVERIFY2(sameValues(seeks(), {35.6, 40.6, 0.3, 0.0, 200.0}), qPrintable(describeValues(seeks())));
    // At the end of a track whose length falls within a second, the value
    // and the description stay within it.
    bridge->setProperty("durationSeconds", 199.8);
    root->setProperty("positionSeconds", 199.6);
    QCOMPARE(announced(seek), QStringLiteral("199 (0 to 199.8), 3:19 of 3:19"));
    bridge->setProperty("durationSeconds", 200.0);

    // The volume in whole percents, from the bridge's, the keys' and a
    // drag's.
    bridge->setProperty("volume", 0.437);
    QCOMPARE(announced(volume), QStringLiteral("44 (0 to 100), 44%"));
    QVERIFY(tabTo(window, volume));
    QTest::keyPress(window, Qt::Key_Right);
    QCOMPARE(announced(volume), QStringLiteral("54 (0 to 100), 54%"));
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY(fuzzyEqual(recordedValues(root, "volumes").constLast(), 0.54));
    const qreal y = volume->height() / 2;
    const auto scenePoint = [volume, y](qreal fraction) {
        return volume->mapToScene(QPointF(volume->width() * fraction, y)).toPoint();
    };
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.5));
    double dragged = 0;
    for (int step = 1; step <= 7; ++step) {
        QTest::mouseMove(window, scenePoint(0.5 - 0.031 * step));
        dragged = volume->property("value").toDouble();
        QVERIFY2(dragged == std::round(dragged), qPrintable(announced(volume)));
        QCOMPARE(announced(volume), QStringLiteral("%1 (0 to 100), %1%").arg(dragged));
    }
    QVERIFY2(dragged > 15.0 && dragged < 40.0, qPrintable(announced(volume)));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.5 - 0.031 * 7));
    QVERIFY(fuzzyEqual(recordedValues(root, "volumes").constLast(), dragged / 100));
}

void AccessibilityTest::arrowKeysStepTheVolumeFromItsValue() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(bridge != nullptr && volume != nullptr);
    QCOMPARE(volume->property("value").toDouble(), 50.0);
    QVERIFY(tabTo(window, volume));
    const auto volumesText = [&root]() {
        return describeValues(recordedValues(root, "volumes"));
    };

    // Right steps up from the bridge's volume, and the bridge gets it.
    QTest::keyClick(window, Qt::Key_Right);
    QVERIFY2(onlyRecordedSince(root, "volumes", 0, 0.6), qPrintable(volumesText()));
    bridge->setProperty("volume", 0.6);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 60.0));

    // Left steps back down.
    const qsizetype sent = recordedValues(root, "volumes").size();
    QTest::keyClick(window, Qt::Key_Left);
    QVERIFY2(onlyRecordedSince(root, "volumes", sent, 0.5), qPrintable(volumesText()));
    bridge->setProperty("volume", 0.5);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 50.0));
}

void AccessibilityTest::bridgeEchoesWaitForTheVolumeGesture() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(bridge != nullptr && volume != nullptr);
    QVERIFY(tabTo(window, volume));
    const auto volumesText = [&root]() {
        return describeValues(recordedValues(root, "volumes"));
    };

    // A held arrow key: a stale echo neither moves the slider nor is sent.
    QTest::keyPress(window, Qt::Key_Right);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 60.0));
    bridge->setProperty("volume", 0.3);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 60.0));
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY2(onlyRecordedSince(root, "volumes", 0, 0.6), qPrintable(volumesText()));
    bridge->setProperty("volume", 0.6);

    // A drag from a quarter to three quarters of the slider, likewise.
    const qsizetype sent = recordedValues(root, "volumes").size();
    const qreal y = volume->height() / 2;
    const auto scenePoint = [volume, y](qreal fraction) {
        return volume->mapToScene(QPointF(volume->width() * fraction, y)).toPoint();
    };
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.25));
    for (int step = 1; step <= 10; ++step) {
        QTest::mouseMove(window, scenePoint(0.25 + 0.05 * step));
    }
    const double dragged = volume->property("value").toDouble();
    QVERIFY2(dragged > 60.0 && dragged < 90.0, qPrintable(QString::number(dragged)));
    bridge->setProperty("volume", 0.05);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), dragged));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.75));
    const QList<double> dragVolumes = recordedValues(root, "volumes").mid(sent);
    QVERIFY2(!dragVolumes.isEmpty() && fuzzyEqual(dragVolumes.constLast(), dragged / 100),
             qPrintable(volumesText()));
    QVERIFY2(std::none_of(dragVolumes.begin(), dragVolumes.end(),
                          [](double value) { return value < 0.15; }),
             qPrintable(volumesText()));
}

void AccessibilityTest::arrowKeysSeekFromThePlaybackPosition() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    QCOMPARE(seek->property("value").toDouble(), 50.0);
    QVERIFY(tabTo(window, seek));
    const auto seeks = [&root]() { return recordedValues(root, "seeks"); };

    // One seek per press, by the step from the playback position.
    QTest::keyClick(window, Qt::Key_Right);
    QCOMPARE(seeks(), (QList<double>{55.0}));
    root->setProperty("positionSeconds", 55.0);
    QCOMPARE(seek->property("value").toDouble(), 55.0);
    QTest::keyClick(window, Qt::Key_Left);
    QCOMPARE(seeks(), (QList<double>{55.0, 50.0}));

    // Playback moves on between presses.
    root->setProperty("positionSeconds", 80.0);
    QTest::keyClick(window, Qt::Key_Right);
    QCOMPARE(seeks(), (QList<double>{55.0, 50.0, 85.0}));

    // While the key is held, the advancing position does not move the
    // slider or the seek.
    root->setProperty("positionSeconds", 85.0);
    QTest::keyPress(window, Qt::Key_Right);
    QCOMPARE(seek->property("value").toDouble(), 90.0);
    root->setProperty("positionSeconds", 86.0);
    QCOMPARE(seek->property("value").toDouble(), 90.0);
    QTest::keyRelease(window, Qt::Key_Right);
    QCOMPARE(seeks(), (QList<double>{55.0, 50.0, 85.0, 90.0}));
}

void AccessibilityTest::seekDragSeeksOnceOnRelease() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    const qreal y = seek->height() / 2;
    const auto scenePoint = [seek, y](qreal fraction) {
        return seek->mapToScene(QPointF(seek->width() * fraction, y)).toPoint();
    };

    // The advancing position does not move the slider during the drag.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.25));
    for (int step = 1; step <= 10; ++step) {
        QTest::mouseMove(window, scenePoint(0.25 + 0.05 * step));
    }
    const double dragged = seek->property("shownSeconds").toDouble();
    QVERIFY2(dragged > 130.0 && dragged < 170.0, qPrintable(QString::number(dragged)));
    root->setProperty("positionSeconds", 51.0);
    QCOMPARE(seek->property("shownSeconds").toDouble(), dragged);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.75));
    QCOMPARE(recordedValues(root, "seeks"), (QList<double>{dragged}));
}

void AccessibilityTest::heldArrowKeysAccumulateVolumeSteps() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(bridge != nullptr && volume != nullptr);
    bridge->setProperty("volume", 0.2);
    root->setProperty("echoLags", true);
    QVERIFY(tabTo(window, volume));

    // Each step is sent once, from the one before, though the echo lags.
    holdKey(window, Qt::Key_Right, 6);
    const QList<double> volumes = recordedValues(root, "volumes");
    QVERIFY2(sameValues(volumes, {0.3, 0.4, 0.5, 0.6, 0.7, 0.8}), qPrintable(describeValues(volumes)));
    bridge->setProperty("volume", 0.8);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 80.0));

    holdKey(window, Qt::Key_Left, 3);
    const QList<double> after = recordedValues(root, "volumes").mid(6);
    QVERIFY2(sameValues(after, {0.7, 0.6, 0.5}), qPrintable(describeValues(after)));
}

void AccessibilityTest::heldArrowKeysSeekOnceWhenReleased() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    root->setProperty("echoLags", true);
    QVERIFY(tabTo(window, seek));
    const auto seeks = [&root]() { return recordedValues(root, "seeks"); };

    // The steps add up, and the hold seeks once, when it ends.
    holdKey(window, Qt::Key_Right, 6);
    QCOMPARE(seeks(), (QList<double>{80.0}));
    root->setProperty("positionSeconds", 80.0);
    holdKey(window, Qt::Key_Left, 3);
    QCOMPARE(seeks(), (QList<double>{80.0, 65.0}));

    // Losing the focus while the key is held ends the hold where it was.
    root->setProperty("positionSeconds", 65.0);
    QTest::keyPress(window, Qt::Key_Right);
    QCOMPARE(seek->property("value").toDouble(), 70.0);
    auto *start = qvariant_cast<QQuickItem *>(root->property("startButton"));
    start->forceActiveFocus(Qt::TabFocusReason);
    QCOMPARE(seeks(), (QList<double>{80.0, 65.0, 70.0}));
    root->setProperty("positionSeconds", 71.0);
    QCOMPARE(seek->property("value").toDouble(), 71.0);
    QTest::keyRelease(window, Qt::Key_Right);
    QCOMPARE(seeks(), (QList<double>{80.0, 65.0, 70.0}));
}

void AccessibilityTest::overlappingArrowKeysAreOneGesture() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(volume != nullptr && seek != nullptr);
    root->setProperty("echoLags", true);

    // Right's release leaves Left held, so the lagging echo does not reset
    // the next step.
    QVERIFY(tabTo(window, volume));
    overlapArrowKeys(window);
    const QList<double> volumes = recordedValues(root, "volumes");
    QVERIFY2(sameValues(volumes, {0.4, 0.3, 0.2, 0.3, 0.2}), qPrintable(describeValues(volumes)));
    QVERIFY(!volume->property("adjusting").toBool());

    // The seek slider seeks once, when the last key is released.
    QVERIFY(tabTo(window, seek));
    overlapArrowKeys(window);
    QCOMPARE(recordedValues(root, "seeks"), (QList<double>{35.0}));
    QVERIFY(!seek->property("adjusting").toBool());
}

void AccessibilityTest::seekDragOutlastsAKeyRelease() {
    TransportHarness harness(TransportHarness::SeekWithLaggingEcho);
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    const qreal y = seek->height() / 2;
    const auto scenePoint = [seek, y](qreal fraction) {
        return seek->mapToScene(QPointF(seek->width() * fraction, y)).toPoint();
    };

    // A key's press and release during a drag leave the drag to the mouse,
    // which seeks once, where it releases the slider.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.25));
    QTest::mouseMove(window, scenePoint(0.3));
    QTest::keyClick(window, Qt::Key_Right);
    for (int step = 1; step <= 4; ++step) {
        QTest::mouseMove(window, scenePoint(0.3 + 0.05 * step));
    }
    QVERIFY(seek->property("adjusting").toBool());
    const double dragged = seek->property("shownSeconds").toDouble();
    QVERIFY2(dragged > 80.0 && dragged < 120.0, qPrintable(QString::number(dragged)));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.5));
    const QList<double> seeks = recordedValues(root, "seeks");
    QVERIFY2(sameValues(seeks, {dragged}), qPrintable(describeValues(seeks)));
    QVERIFY(!seek->property("adjusting").toBool());
}

void AccessibilityTest::seekDragOutlastsTheFocus() {
    TransportHarness harness(TransportHarness::SeekWithLaggingEcho);
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    auto *start = qvariant_cast<QQuickItem *>(root->property("startButton"));
    const qreal y = seek->height() / 2;
    const auto scenePoint = [seek, y](qreal fraction) {
        return seek->mapToScene(QPointF(seek->width() * fraction, y)).toPoint();
    };
    const auto seeksText = [&root]() {
        return describeValues(recordedValues(root, "seeks"));
    };

    // The focus leaves while a key is held: the drag still seeks once, to
    // where the mouse releases the slider.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.4));
    QTest::mouseMove(window, scenePoint(0.45));
    QTest::keyPress(window, Qt::Key_Right);
    start->forceActiveFocus(Qt::TabFocusReason);
    QVERIFY(start->hasActiveFocus());
    for (int step = 1; step <= 6; ++step) {
        QTest::mouseMove(window, scenePoint(0.45 + 0.05 * step));
    }
    QVERIFY(seek->property("adjusting").toBool());
    const double dragged = seek->property("shownSeconds").toDouble();
    QVERIFY2(dragged > 130.0 && dragged < 170.0, qPrintable(QString::number(dragged)));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.75));
    QVERIFY2(sameValues(recordedValues(root, "seeks"), {dragged}), qPrintable(seeksText()));
    QVERIFY(!seek->property("adjusting").toBool());

    // The key's release, which reaches the button, seeks nothing more.
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY2(sameValues(recordedValues(root, "seeks"), {dragged}), qPrintable(seeksText()));
}

void AccessibilityTest::seekClickSeeksOnce() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      seek->mapToScene(QPointF(seek->width() * 0.75, seek->height() / 2)).toPoint());
    const QList<double> seeks = recordedValues(root, "seeks");
    QVERIFY2(seeks.size() == 1 && seeks.constFirst() > 130.0 && seeks.constFirst() < 170.0,
             qPrintable(describeValues(seeks)));
}

namespace {

// The time the seek slider maps `x`, in its coordinates, to.
double secondsAtX(QQuickItem *seek, qreal x) {
    QVariant result;
    if (!QMetaObject::invokeMethod(seek, "secondsAtX", Q_RETURN_ARG(QVariant, result),
                                   Q_ARG(QVariant, x))) {
        return -1;
    }
    return result.toDouble();
}

// A point on the seek slider, in the whole pixels a mouse gives, and its time.
struct SeekPoint {
    QPoint scene;
    double seconds = 0;
};

// The first point from `fraction` of the slider's width whose time is well
// between two of the keyboard's 5-second steps.
std::optional<SeekPoint> pointBetweenSteps(QQuickItem *seek, qreal fraction) {
    const qreal y = seek->height() / 2;
    for (int x = qRound(seek->width() * fraction); x < seek->width(); ++x) {
        const QPoint scene = seek->mapToScene(QPointF(x, y)).toPoint();
        const double seconds = secondsAtX(seek, seek->mapFromScene(QPointF(scene)).x());
        const double intoStep = std::fmod(seconds, 5.0);
        if (intoStep > 1.5 && intoStep < 3.5) {
            return SeekPoint{scene, seconds};
        }
    }
    return std::nullopt;
}

// The seek bar's time: a native tooltip above the bar, over the line it
// names.
struct SeekTimeBox {
    QObject *box = nullptr;

    bool find(QObject *root) {
        box = root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
        return box != nullptr && box->inherits("QQuickToolTip");
    }
    // The time it shows, or an empty text when it is hidden.
    QString shown() const {
        return box->property("shown").toBool() ? box->property("shownText").toString() : QString();
    }
    // Its text while hidden: none.
    QString hiddenText() const {
        return box->property("shownText").toString();
    }
    qreal x() const {
        return box->property("x").toReal();
    }
    qreal width() const {
        return box->property("width").toReal();
    }
};

// Why `box` is not over `anchorX`, in the slider's coordinates, as drawn:
// above the bar, its bottom edge on the bar's top, so it covers none of the
// waveform and nothing under the bar; centred on the line, to within the
// device pixel it is rounded to, or against the bar's end where centring
// would take it past it; or an empty text.
QString timeBoxPlacementError(QQuickItem *seek, const SeekTimeBox &box, qreal anchorX, qreal dpr) {
    auto *content = box.box->property("contentItem").value<QQuickItem *>();
    QQuickItem *popup = content != nullptr ? content->parentItem() : nullptr;
    // A hover's time shows after a moment.
    if (popup == nullptr || !QTest::qWaitFor([&]() { return popup->isVisible(); })) {
        return QStringLiteral("not drawn");
    }
    const QRectF drawn = popup->mapRectToScene(QRectF(0, 0, popup->width(), popup->height()));
    const QRectF bar = seek->mapRectToScene(QRectF(0, 0, seek->width(), seek->height()));
    const qreal left = drawn.left() - bar.left();
    const qreal right = drawn.right() - bar.left();
    const qreal slack = 0.5 / dpr + 1e-6;
    QStringList errors;
    const bool centred = std::abs((left + right) / 2 - anchorX) <= slack;
    const bool atStart = std::abs(left) <= slack && anchorX < drawn.width() / 2 + slack;
    const bool atEnd = std::abs(right - bar.width()) <= slack && anchorX > bar.width() - drawn.width() / 2 - slack;
    if (!centred && !atStart && !atEnd) {
        errors.push_back(QStringLiteral("from %1 to %2, not over %3").arg(left).arg(right).arg(anchorX));
    }
    if (left > anchorX + slack || right < anchorX - slack) {
        errors.push_back(QStringLiteral("not over the line at %1").arg(anchorX));
    }
    if (!fuzzyEqual(drawn.left() * dpr, std::round(drawn.left() * dpr))) {
        errors.push_back(QStringLiteral("at %1 device pixels").arg(drawn.left() * dpr));
    }
    if (std::abs(drawn.bottom() - bar.top()) > slack) {
        errors.push_back(QStringLiteral("its bottom at %1, not on the bar's top at %2")
                             .arg(drawn.bottom()).arg(bar.top()));
    }
    return errors.join(QStringLiteral("; "));
}

// `seconds` as the transport bar shows a time under an hour.
QString minutesAndSeconds(double seconds) {
    const int whole = static_cast<int>(std::floor(seconds));
    return QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

void AccessibilityTest::seekDragShowsAndSeeksTheTimeUnderThePointer() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    SeekTimeBox dragTime;
    QVERIFY(seek != nullptr && dragTime.find(window));
    const std::optional<SeekPoint> target = pointBetweenSteps(seek, 0.6);
    QVERIFY(target.has_value());
    const qreal y = seek->height() / 2;

    // Between the keyboard's steps, the drag shows the second under the
    // pointer, and its value is the time there.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                      seek->mapToScene(QPointF(seek->width() * 0.25, y)).toPoint());
    QTest::mouseMove(window, seek->mapToScene(QPointF(seek->width() * 0.4, y)).toPoint());
    QTest::mouseMove(window, target->scene);
    QVERIFY(seek->property("adjusting").toBool());
    const double dragged = seek->property("shownSeconds").toDouble();
    QVERIFY2(fuzzyEqual(dragged, target->seconds),
             qPrintable(QStringLiteral("%1 vs %2").arg(dragged).arg(target->seconds)));
    QCOMPARE(dragTime.shown(), minutesAndSeconds(target->seconds));
    QCOMPARE(accessibleText(seek, QAccessible::Description),
             QStringLiteral("%1 of 3:20").arg(minutesAndSeconds(target->seconds)));

    // The release seeks there.
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target->scene);
    const QList<double> seeks = recordedValues(root, "seeks");
    QVERIFY2(sameValues(seeks, {target->seconds}), qPrintable(describeValues(seeks)));
}

void AccessibilityTest::seekClickSeeksToTheTimeUnderThePointer() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    const std::optional<SeekPoint> target = pointBetweenSteps(seek, 0.3);
    QVERIFY(target.has_value());
    QAccessibleValueInterface *value = QAccessible::queryAccessibleInterface(seek)->valueInterface();
    QVERIFY(value != nullptr);
    QCOMPARE(value->minimumStepSize().toDouble(), 5.0);

    // A click between the keyboard's steps seeks to the time under it.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, target->scene);
    QList<double> seeks = recordedValues(root, "seeks");
    QVERIFY2(sameValues(seeks, {target->seconds}), qPrintable(describeValues(seeks)));

    // The arrow keys still step by the keyboard's step, from the playback
    // position.
    QCOMPARE(value->minimumStepSize().toDouble(), 5.0);
    QVERIFY(tabTo(window, seek));
    QTest::keyClick(window, Qt::Key_Right);
    root->setProperty("positionSeconds", 55.0);
    QTest::keyClick(window, Qt::Key_Right);
    seeks = recordedValues(root, "seeks");
    QVERIFY2(sameValues(seeks, {target->seconds, 55.0, 60.0}), qPrintable(describeValues(seeks)));
}

void AccessibilityTest::seekPointerReachesEitherEndOnItsWholeSecond() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    QAccessibleValueInterface *value = QAccessible::queryAccessibleInterface(seek)->valueInterface();
    QVERIFY(value != nullptr);
    const qreal y = seek->height() / 2;
    // The point at `x` on the slider, in the whole pixels a mouse gives.
    const auto pointAt = [seek, y](qreal x) {
        const QPoint scene = seek->mapToScene(QPointF(x, y)).toPoint();
        return SeekPoint{scene, secondsAtX(seek, seek->mapFromScene(QPointF(scene)).x())};
    };
    const auto pointWithin = [&](double from, double to) -> std::optional<SeekPoint> {
        for (int x = 0; x < seek->width(); ++x) {
            const SeekPoint point = pointAt(x);
            if (point.seconds > from && point.seconds < to) {
                return point;
            }
        }
        return std::nullopt;
    };
    const SeekPoint start = pointAt(0);
    const SeekPoint end = pointAt(seek->width() - 1);
    const std::optional<SeekPoint> nearStart = pointWithin(0.0, 0.5);
    const std::optional<SeekPoint> nearEnd = pointWithin(199.5, 200.0);
    QCOMPARE(start.seconds, 0.0);
    QCOMPARE(end.seconds, 200.0);
    QVERIFY(nearStart.has_value() && nearEnd.has_value());
    // While the pointer holds the slider: the time it shows, and the
    // value assistive technology reads, a whole second.
    const auto held = [seek, value]() {
        return QStringLiteral("%1, value %2")
            .arg(seek->property("shownSeconds").toDouble())
            .arg(value->currentValue().toDouble());
    };
    const auto heldAt = [](double seconds) {
        return QStringLiteral("%1, value %2").arg(seconds).arg(std::floor(seconds));
    };
    const auto seeks = [&root]() { return recordedValues(root, "seeks"); };

    // Within half a second of the start, the slider's value is already
    // there, and a click at the start seeks to it.
    root->setProperty("positionSeconds", 0.3);
    QCOMPARE(value->currentValue().toDouble(), 0.0);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start.scene);
    QVERIFY(seek->property("adjusting").toBool());
    QCOMPARE(held(), heldAt(0.0));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start.scene);
    QVERIFY2(sameValues(seeks(), {0.0}), qPrintable(describeValues(seeks())));

    // So does a drag from within that half second to the start.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, nearStart->scene);
    QCOMPARE(held(), heldAt(nearStart->seconds));
    QTest::mouseMove(window, start.scene);
    QCOMPARE(held(), heldAt(0.0));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start.scene);
    QVERIFY2(sameValues(seeks(), {0.0, 0.0}), qPrintable(describeValues(seeks())));

    // Within half a second of the end, a click at the end, or a drag past
    // it, seeks to the length.
    root->setProperty("positionSeconds", 199.7);
    QCOMPARE(value->currentValue().toDouble(), 200.0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, end.scene);
    QVERIFY2(sameValues(seeks(), {0.0, 0.0, 200.0}), qPrintable(describeValues(seeks())));
    const QPoint pastEnd = end.scene + QPoint(20, 0);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, nearEnd->scene);
    QCOMPARE(held(), heldAt(nearEnd->seconds));
    QTest::mouseMove(window, pastEnd);
    QCOMPARE(held(), heldAt(200.0));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, pastEnd);
    QVERIFY2(sameValues(seeks(), {0.0, 0.0, 200.0, 200.0}), qPrintable(describeValues(seeks())));

    // A touch, which the slider takes where it lifts, as it takes a
    // touch's drag only once it moves: here, at the start, with no update
    // between, so only the release has its place.
    root->setProperty("positionSeconds", 0.3);
    QPointingDevice *touchScreen = QTest::createTouchDevice();
    QTest::touchEvent(window, touchScreen).press(0, nearStart->scene, window);
    QVERIFY(seek->property("adjusting").toBool());
    QCOMPARE(held(), heldAt(0.3));
    QTest::touchEvent(window, touchScreen).release(0, start.scene, window);
    QVERIFY2(sameValues(seeks(), {0.0, 0.0, 200.0, 200.0, 0.0}), qPrintable(describeValues(seeks())));
}

void AccessibilityTest::playbackMovesTheSeekBarByWholePixels() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *line = visibleItemNamed(window, QStringLiteral("seekPlayheadLine"));
    QQuickItem *progress = visibleItemNamed(window, QStringLiteral("seekUnplayed"));
    QVERIFY(seek != nullptr && line != nullptr && progress != nullptr);
    int frames = 0;
    const FrameSampler counting(window, [&frames]() { ++frames; });
    QTRY_VERIFY(frames > 0);
    // Waits until the window draws no more frames, for up to five seconds.
    const auto settle = [&frames]() {
        int seen = -1;
        for (int wait = 0; wait < 50 && seen != frames; ++wait) {
            seen = frames;
            QTest::qWait(100);
        }
        return seen == frames;
    };
    QVERIFY(settle());

    // Three seconds of playback in 90 position updates, a frame apart: twice
    // as fast as playback, which moves the bar about 3 device pixels a
    // second at 1x, and the time label once.
    const double startSeconds = 50.0;
    const double durationSeconds = 200.0;
    const int steps = 90;
    const double secondsPerStep = 2.0 / 60.0;
    struct Drawn {
        qreal lineX;
        qreal progressWidth;
        QString positionText;
        bool operator==(const Drawn &) const = default;
    };
    const auto drawn = [&]() {
        return Drawn{line->mapToItem(progress->parentItem(), QPointF()).x(), progress->x(),
                     root->property("positionText").toString()};
    };
    Drawn previous = drawn();
    int changes = 0;
    frames = 0;
    for (int step = 1; step <= steps; ++step) {
        root->setProperty("positionSeconds", startSeconds + step * secondsPerStep);
        const Drawn now = drawn();
        changes += now == previous ? 0 : 1;
        previous = now;
        QTest::qWait(16);
    }
    QVERIFY(settle());
    const QString counts = QStringLiteral("%1 frames, %2 drawn changes").arg(frames).arg(changes);
    qInfo("seek bar playback: %s in %d position updates", qPrintable(counts), steps);
    QVERIFY2(changes >= 3 && changes < steps / 4 && frames <= changes, qPrintable(counts));

    // The value stays exact, and the bar is within a device pixel of it:
    // the unplayed part starts at the playhead's line, which moves over the bar less
    // its own width.
    const double endSeconds = startSeconds + steps * secondsPerStep;
    QVERIFY(fuzzyEqual(seek->property("shownSeconds").toDouble(), endSeconds));
    const qreal devicePixel = 1.0 / window->effectiveDevicePixelRatio();
    const qreal travel = progress->parentItem()->width() - line->width();
    const qreal exactWidth = travel * endSeconds / durationSeconds;
    QVERIFY2(std::abs(progress->x() - exactWidth) <= devicePixel,
             qPrintable(QStringLiteral("%1 vs %2").arg(progress->x()).arg(exactWidth)));

    // A held key draws its value to the nearest device pixel.
    QVERIFY(tabTo(window, seek));
    QTest::keyPress(window, Qt::Key_Right);
    QVERIFY(seek->property("adjusting").toBool());
    QVERIFY(fuzzyEqual(seek->property("shownSeconds").toDouble(), endSeconds + 5.0));
    QVERIFY2(std::abs(progress->x() - travel * (endSeconds + 5.0) / durationSeconds) <= devicePixel / 2,
             qPrintable(QString::number(progress->x())));
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY(sameValues(recordedValues(root, "seeks"), {endSeconds + 5.0}));

    // A wheel step seeks at once, as the desktop style's wheel does, and the
    // bar draws the seek's position.
    root->setProperty("positionSeconds", endSeconds + 5.0);
    seek->setProperty("wheelEnabled", true);
    QVERIFY(sendWheel(window, seek->mapToScene(QPointF(seek->width() / 2, seek->height() / 2)),
                      QPoint(0, 120)));
    const QList<double> seeks = recordedValues(root, "seeks");
    QVERIFY2(sameValues(seeks, {endSeconds + 5.0, endSeconds + 10.0}), qPrintable(describeValues(seeks)));
    root->setProperty("positionSeconds", endSeconds + 10.0);
    QVERIFY(std::abs(progress->x() - travel * (endSeconds + 10.0) / durationSeconds) <= devicePixel);
}

namespace {

// What is wrong with the seek bar's playhead as drawn at `dpr` device pixels
// to a logical one, or nothing. Its line is two logical pixels wide and its
// outline one on each side where the bar has room, each a whole number of
// device pixels; the line's left edge is on a whole device pixel of the bar,
// and the unplayed part starts there. The line and its outline are on the bar.
QString playheadGeometryError(QQuickItem *line, QQuickItem *progress, qreal dpr) {
    QQuickItem *outline = line->parentItem();
    QQuickItem *bar = progress->parentItem();
    const qreal lineX = line->mapToItem(bar, QPointF()).x();
    const qreal lineWidth = std::max<qreal>(1.0, std::round(2.0 * dpr)) / dpr;
    const qreal outlineWidth = std::max<qreal>(1.0, std::round(dpr)) / dpr;
    const qreal outlineLeft = std::max<qreal>(0.0, lineX - outlineWidth);
    const qreal outlineRight = std::min(bar->width(), lineX + lineWidth + outlineWidth);
    QStringList errors;
    if (!fuzzyEqual(line->width(), lineWidth)) {
        errors.push_back(QStringLiteral("line %1 wide, not %2").arg(line->width()).arg(lineWidth));
    }
    if (!fuzzyEqual(lineX * dpr, std::round(lineX * dpr))) {
        errors.push_back(QStringLiteral("line at %1 device pixels").arg(lineX * dpr));
    }
    if (!fuzzyEqual(progress->x(), lineX)
        || !fuzzyEqual(progress->x() + progress->width(), bar->width())) {
        errors.push_back(QStringLiteral("unplayed part from %1 to %2, line at %3")
                             .arg(progress->x()).arg(progress->x() + progress->width()).arg(lineX));
    }
    if (!fuzzyEqual(outline->x(), outlineLeft) || !fuzzyEqual(outline->x() + outline->width(), outlineRight)) {
        errors.push_back(QStringLiteral("outline from %1 to %2, not %3 to %4")
                             .arg(outline->x()).arg(outline->x() + outline->width())
                             .arg(outlineLeft).arg(outlineRight));
    }
    // Across the 28 px bar.
    if (!fuzzyEqual(bar->height(), 28.0) || !fuzzyEqual(line->height(), 28.0)
        || !fuzzyEqual(outline->height(), 28.0) || !fuzzyEqual(outline->y(), 0.0)) {
        errors.push_back(QStringLiteral("the playhead is %1 px at %2, not across the 28 px bar")
                             .arg(outline->height()).arg(outline->y()));
    }
    constexpr qreal kTolerance = 1e-6;
    const qreal lineRight = lineX + line->width();
    if (lineX < -kTolerance || lineRight > bar->width() + kTolerance) {
        errors.push_back(QStringLiteral("line from %1 to %2 off the bar of %3")
                             .arg(lineX).arg(lineRight).arg(bar->width()));
    }
    if (outline->x() < -kTolerance || outline->x() + outline->width() > bar->width() + kTolerance) {
        errors.push_back(QStringLiteral("outline from %1 to %2 off the bar of %3")
                             .arg(outline->x()).arg(outline->x() + outline->width()).arg(bar->width()));
    }
    return errors.join(QStringLiteral("; "));
}

} // namespace

void AccessibilityTest::seekBarStepsByTheWindowsDevicePixels() {
    // A window drawn at 1.25 device pixels to a logical one on a screen at
    // one, as fractional scaling can make it.
    QQuickRenderControl control;
    QQuickWindow window(&control);
    QImage canvas(16, 16, QImage::Format_ARGB32_Premultiplied);
    QQuickRenderTarget renderTarget = QQuickRenderTarget::fromPaintDevice(&canvas);
    renderTarget.setDevicePixelRatio(1.25);
    window.setRenderTarget(renderTarget);
    QCOMPARE(window.effectiveDevicePixelRatio(), 1.25);
    QCOMPARE(window.screen()->devicePixelRatio(), 1.0);

    // The transport harness as an item of that window.
    QQmlEngine engine;
    QString errorText;
    QByteArray source = transportHarnessQml();
    QVERIFY(source.contains("ApplicationWindow {"));
    source.replace("ApplicationWindow {", "Control {");
    QScopedPointer<QObject> root(createQml(engine, source, &errorText));
    auto *harness = qobject_cast<QQuickItem *>(root.data());
    QVERIFY2(harness != nullptr, qPrintable(errorText));
    harness->setParentItem(window.contentItem());
    control.polishItems();
    QQuickItem *progress = itemsNamed(harness, QStringLiteral("seekUnplayed")).value(0);
    QQuickItem *line = itemsNamed(harness, QStringLiteral("seekPlayheadLine")).value(0);
    QVERIFY(progress != nullptr && line != nullptr && progress->parentItem()->width() > 500);

    // Each step the bar takes in five seconds of playback is one of the
    // window's device pixels, and the playhead keeps its device pixels' size
    // and stays on whole ones, with the unplayed part after it.
    const auto checkSteps = [&](double fromSeconds) {
        root->setProperty("positionSeconds", fromSeconds);
        QList<qreal> steps;
        qreal width = progress->x();
        for (int step = 1; step <= 250; ++step) {
            root->setProperty("positionSeconds", fromSeconds + step / 50.0);
            const QString geometryError =
                playheadGeometryError(line, progress, window.effectiveDevicePixelRatio());
            QVERIFY2(geometryError.isEmpty(),
                     qPrintable(QStringLiteral("at %1: %2")
                                    .arg(window.effectiveDevicePixelRatio()).arg(geometryError)));
            if (progress->x() != width) {
                steps.push_back((progress->x() - width) * window.effectiveDevicePixelRatio());
                width = progress->x();
            }
        }
        QStringList stepsText;
        for (const qreal step : steps) {
            stepsText.push_back(QString::number(step));
        }
        QVERIFY2(steps.size() >= 10 && std::all_of(steps.begin(), steps.end(),
                     [](qreal step) { return std::abs(step - 1.0) < 0.01; }),
                 qPrintable(QStringLiteral("at %1: %2")
                                .arg(window.effectiveDevicePixelRatio())
                                .arg(stepsText.join(QStringLiteral(", ")))));
    };
    checkSteps(50.0);
    if (QTest::currentTestFailed()) {
        return;
    }

    // The window's ratio changes, as when it moves to another screen.
    renderTarget.setDevicePixelRatio(2.0);
    window.setRenderTarget(renderTarget);
    QEvent changed(QEvent::DevicePixelRatioChange);
    QCoreApplication::sendEvent(&window, &changed);
    checkSteps(60.0);
}

void AccessibilityTest::seekBarPlayheadIsOneOutlinedLine() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    const qreal dpr = window->effectiveDevicePixelRatio();
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *progress = visibleItemNamed(window, QStringLiteral("seekUnplayed"));
    QVERIFY(seek != nullptr && progress != nullptr);

    // One marker: a line in its outline. No other playhead or handle draws.
    const QList<QQuickItem *> outlines = itemsNamed(window->contentItem(), QStringLiteral("seekPlayhead"));
    const QList<QQuickItem *> lines = itemsNamed(window->contentItem(), QStringLiteral("seekPlayheadLine"));
    QCOMPARE(outlines.size(), 1);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.first()->parentItem(), outlines.first());
    QVERIFY(outlines.first()->isVisible() && lines.first()->isVisible());
    QVERIFY(itemsNamed(window->contentItem(), QStringLiteral("seekHandle")).isEmpty());
    QQuickItem *outline = outlines.first();
    QQuickItem *line = lines.first();
    QQuickItem *bar = progress->parentItem();
    QCOMPARE(line->property("antialiasing").toBool(), false);
    QCOMPARE(outline->property("antialiasing").toBool(), false);

    // Two logical pixels wide, with one on each side, at the position.
    QCOMPARE(dpr, 1.0);
    QString geometryError = playheadGeometryError(line, progress, dpr);
    QVERIFY2(geometryError.isEmpty(), qPrintable(geometryError));
    QCOMPARE(line->width(), 2.0);
    QCOMPARE(line->x(), 1.0);
    QCOMPARE(outline->width(), 4.0);
    const qreal travel = bar->width() - line->width();
    QVERIFY(std::abs(progress->x() - travel * 50.0 / 200.0) <= 0.5);

    // The grab target is wider than the marker and centred on its line, and
    // it draws nothing.
    QQuickItem *grab = qvariant_cast<QQuickItem *>(seek->property("handle"));
    QVERIFY(grab != nullptr);
    QCOMPARE(grab->objectName(), QStringLiteral("seekGrabTarget"));
    QVERIFY2(grab->width() > outline->width(), qPrintable(QString::number(grab->width())));
    QVERIFY(!grab->flags().testFlag(QQuickItem::ItemHasContents));
    QVERIFY(grab->childItems().isEmpty());
    const auto lineCentre = [&]() { return line->mapToItem(seek, QPointF(line->width() / 2, 0)).x(); };
    QCOMPARE(grab->mapToItem(seek, QPointF(grab->width() / 2, 0)).x(), lineCentre());

    // A press on the line's centre is at the time the line shows, to within
    // the step of a device pixel.
    const double stepSeconds = 200.0 / seek->property("travelDevicePixels").toDouble();
    QVERIFY(std::abs(secondsAtX(seek, lineCentre()) - 50.0) <= stepSeconds / 2 + 1e-9);

    // At either end, the line stays on the bar and its outline is cut to it.
    root->setProperty("positionSeconds", 0.0);
    geometryError = playheadGeometryError(line, progress, dpr);
    QVERIFY2(geometryError.isEmpty(), qPrintable(geometryError));
    QCOMPARE(line->mapToItem(bar, QPointF()).x(), 0.0);
    QCOMPARE(outline->x(), 0.0);
    QCOMPARE(progress->x(), 0.0);
    QCOMPARE(grab->mapToItem(seek, QPointF(grab->width() / 2, 0)).x(), lineCentre());
    root->setProperty("positionSeconds", 200.0);
    geometryError = playheadGeometryError(line, progress, dpr);
    QVERIFY2(geometryError.isEmpty(), qPrintable(geometryError));
    const qreal lineRight = line->mapToItem(bar, QPointF(line->width(), 0)).x();
    QVERIFY2(std::abs(lineRight - bar->width()) <= 0.5 && outline->x() + outline->width() <= bar->width(),
             qPrintable(QStringLiteral("%1 of %2").arg(lineRight).arg(bar->width())));
    QCOMPARE(grab->mapToItem(seek, QPointF(grab->width() / 2, 0)).x(), lineCentre());
}

void AccessibilityTest::seekBarPlayheadStaysOnTheBarAtEitherEnd() {
    // The transport harness as an item of a window drawn at fractional and
    // whole device pixel ratios, as in seekBarStepsByTheWindowsDevicePixels.
    QQuickRenderControl control;
    QQuickWindow window(&control);
    QImage canvas(16, 16, QImage::Format_ARGB32_Premultiplied);
    QQuickRenderTarget renderTarget = QQuickRenderTarget::fromPaintDevice(&canvas);
    renderTarget.setDevicePixelRatio(1.25);
    window.setRenderTarget(renderTarget);
    QQmlEngine engine;
    QString errorText;
    QByteArray source = transportHarnessQml();
    QVERIFY(source.contains("ApplicationWindow {"));
    source.replace("ApplicationWindow {", "Control {");
    QScopedPointer<QObject> root(createQml(engine, source, &errorText));
    auto *harness = qobject_cast<QQuickItem *>(root.data());
    QVERIFY2(harness != nullptr, qPrintable(errorText));
    harness->setParentItem(window.contentItem());
    control.polishItems();
    QQuickItem *progress = itemsNamed(harness, QStringLiteral("seekUnplayed")).value(0);
    QQuickItem *line = itemsNamed(harness, QStringLiteral("seekPlayheadLine")).value(0);
    QVERIFY(progress != nullptr && line != nullptr);
    QQuickItem *bar = progress->parentItem();

    for (const qreal dpr : {1.25, 1.5, 2.0}) {
        renderTarget.setDevicePixelRatio(dpr);
        window.setRenderTarget(renderTarget);
        QEvent changed(QEvent::DevicePixelRatioChange);
        QCoreApplication::sendEvent(&window, &changed);
        QCOMPARE(window.effectiveDevicePixelRatio(), dpr);
        QSet<qint64> barWidths;
        int oddBars = 0;
        // Harness widths, some fractional, that give the bar even and odd
        // widths, three or more of each whatever the width of the time
        // beside it, which follows the font; the layout gives it whole
        // logical pixels.
        for (const qreal width :
             {1000.0, 1001.0, 1002.0, 1003.0, 1004.5, 1005.25, 1006.75, 1007.0, 1008.0}) {
            harness->setWidth(width);
            control.polishItems();
            const qreal barWidth = bar->width();
            const auto wholeWidth = static_cast<qint64>(std::round(barWidth));
            if (!barWidths.contains(wholeWidth) && fuzzyEqual(barWidth, static_cast<qreal>(wholeWidth))) {
                oddBars += wholeWidth % 2 == 1 ? 1 : 0;
            }
            barWidths.insert(wholeWidth);
            for (const double seconds : {0.0, 200.0}) {
                root->setProperty("positionSeconds", seconds);
                const QString geometryError = playheadGeometryError(line, progress, dpr);
                QVERIFY2(geometryError.isEmpty(),
                         qPrintable(QStringLiteral("at %1, bar %2, %3 s: %4")
                                        .arg(dpr).arg(barWidth).arg(seconds).arg(geometryError)));
            }
            // At the end, the line's right edge is within a device pixel of
            // the bar's end.
            const qreal lineRight = line->mapToItem(bar, QPointF(line->width(), 0)).x();
            QVERIFY2(barWidth - lineRight < 1.0 / dpr - 1e-6,
                     qPrintable(QStringLiteral("at %1, bar %2: line ends at %3")
                                    .arg(dpr).arg(barWidth).arg(lineRight)));
        }
        QVERIFY2(barWidths.size() >= 6 && oddBars >= 3,
                 qPrintable(QStringLiteral("at %1: %2 bar widths, %3 odd").arg(dpr).arg(barWidths.size()).arg(oddBars)));
    }
}

namespace {

// Sends a mouse event at `scene`, as the window system does; a move with
// no button held hovers.
void sendMouse(QQuickWindow *window, QEvent::Type type, QPointF scene, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent event(type, scene, scene, window->mapToGlobal(scene), button, buttons, Qt::NoModifier,
                      QPointingDevice::primaryPointingDevice());
    QSpontaneKeyEvent::setSpontaneous(&event);
    QCoreApplication::sendEvent(window, &event);
}

} // namespace

void AccessibilityTest::seekHoverShowsTheTimeUnderThePointer() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    const qreal dpr = window->effectiveDevicePixelRatio();
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *progress = visibleItemNamed(window, QStringLiteral("seekUnplayed"));
    SeekTimeBox timeBox;
    QVERIFY(seek != nullptr && progress != nullptr && timeBox.find(window));
    QCOMPARE(timeBox.shown(), QString());
    const qreal playedWidth = progress->x();
    const qreal y = seek->height() / 2;
    const std::optional<SeekPoint> between = pointBetweenSteps(seek, 0.45);
    QVERIFY(between.has_value());

    // Across the bar and at either end, the tooltip shows the second under
    // the pointer, over it.
    struct Expected {
        qreal x;
        QString time;
    };
    const qreal betweenX = seek->mapFromScene(QPointF(between->scene)).x();
    for (const Expected &expected : {Expected{0, QStringLiteral("0:00")}, Expected{1, QString()},
                                     Expected{3, QString()}, Expected{betweenX, QString()},
                                     Expected{seek->width() * 0.8, QString()},
                                     Expected{seek->width() - 1, QStringLiteral("3:20")}}) {
        const QPoint scene = seek->mapToScene(QPointF(expected.x, y)).toPoint();
        const qreal x = seek->mapFromScene(QPointF(scene)).x();
        QTest::mouseMove(window, scene);
        const QString time = minutesAndSeconds(secondsAtX(seek, x));
        QVERIFY2(timeBox.shown() == time, qPrintable(QStringLiteral("at %1: %2, not %3")
                                                         .arg(x).arg(timeBox.shown(), time)));
        if (!expected.time.isEmpty()) {
            QCOMPARE(time, expected.time);
        }
        const QString placementError = timeBoxPlacementError(seek, timeBox, x, dpr);
        QVERIFY2(placementError.isEmpty(), qPrintable(QStringLiteral("at %1: %2").arg(x).arg(placementError)));
    }

    // Hovering moves neither the slider nor the playhead, and seeks nothing.
    QCOMPARE(seek->property("value").toDouble(), 50.0);
    QCOMPARE(progress->x(), playedWidth);
    QVERIFY(!seek->property("adjusting").toBool());
    QVERIFY2(recordedValues(root, "seeks").isEmpty(),
             qPrintable(describeValues(recordedValues(root, "seeks"))));

    // Leaving the bar hides the box; coming back shows it again.
    QTest::mouseMove(window, centerOf(qvariant_cast<QQuickItem *>(root->property("startButton"))));
    QCOMPARE(timeBox.shown(), QString());
    QCOMPARE(timeBox.hiddenText(), QString());
    QTest::mouseMove(window, between->scene);
    QCOMPARE(timeBox.shown(), minutesAndSeconds(between->seconds));
    QTest::mouseMove(window, seek->mapToScene(QPointF(seek->width() + 4, y)).toPoint());
    QCOMPARE(timeBox.shown(), QString());
    QVERIFY(recordedValues(root, "seeks").isEmpty());
}

void AccessibilityTest::seekHoverShowsTheTimeAClickSeeksTo() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    SeekTimeBox timeBox;
    QVERIFY(seek != nullptr && timeBox.find(window));
    const qreal y = seek->height() / 2;

    qsizetype clicks = 0;
    for (const qreal fraction : {0.0, 0.17, 0.5, 0.731, 1.0}) {
        const qreal x = std::min(seek->width() * fraction, seek->width() - 1);
        const QPoint scene = seek->mapToScene(QPointF(x, y)).toPoint();
        QTest::mouseMove(window, scene);
        const QString hovered = timeBox.shown();
        QVERIFY2(!hovered.isEmpty(), qPrintable(QStringLiteral("at %1").arg(x)));
        QCOMPARE(recordedValues(root, "seeks").size(), clicks);

        // A click there seeks to the time the box showed, and the box,
        // under the pointer still, shows it again.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, scene);
        ++clicks;
        const QList<double> seeks = recordedValues(root, "seeks");
        QCOMPARE(seeks.size(), clicks);
        QVERIFY2(minutesAndSeconds(seeks.constLast()) == hovered,
                 qPrintable(QStringLiteral("at %1: seeks to %2, shows %3")
                                .arg(x).arg(seeks.constLast()).arg(hovered)));
        QCOMPARE(timeBox.shown(), hovered);
    }
}

void AccessibilityTest::seekHoverFollowsTheWindowsDevicePixels() {
    // The transport harness as an item of a window drawn at fractional and
    // whole device pixel ratios, as in seekBarStepsByTheWindowsDevicePixels.
    QQuickRenderControl control;
    QQuickWindow window(&control);
    // Big enough to hold the harness, which hover is delivered within.
    window.resize(1000, 200);
    QImage canvas(16, 16, QImage::Format_ARGB32_Premultiplied);
    QQuickRenderTarget renderTarget = QQuickRenderTarget::fromPaintDevice(&canvas);
    renderTarget.setDevicePixelRatio(1.25);
    window.setRenderTarget(renderTarget);
    QQmlEngine engine;
    QString errorText;
    QByteArray source = transportHarnessQml();
    QVERIFY(source.contains("ApplicationWindow {"));
    source.replace("ApplicationWindow {", "Control {");
    QScopedPointer<QObject> root(createQml(engine, source, &errorText));
    auto *harness = qobject_cast<QQuickItem *>(root.data());
    QVERIFY2(harness != nullptr, qPrintable(errorText));
    harness->setParentItem(window.contentItem());
    control.polishItems();
    QQuickItem *seek = itemsNamed(harness, QStringLiteral("seekSlider")).value(0);
    SeekTimeBox timeBox;
    QVERIFY(seek != nullptr && timeBox.find(harness) && seek->width() > 500);
    const qreal y = seek->height() / 2;
    const auto hover = [&](qreal x) {
        sendMouse(&window, QEvent::MouseMove, seek->mapToScene(QPointF(x, y)), Qt::NoButton, Qt::NoButton);
    };

    qsizetype clicks = 0;
    for (const qreal dpr : {1.25, 2.0}) {
        renderTarget.setDevicePixelRatio(dpr);
        window.setRenderTarget(renderTarget);
        QEvent changed(QEvent::DevicePixelRatioChange);
        QCoreApplication::sendEvent(&window, &changed);
        QCOMPARE(window.effectiveDevicePixelRatio(), dpr);
        const qreal width = seek->width();

        // At fractional pointer positions, and at either end, the box shows
        // the time a click there seeks to, over the pointer.
        for (const qreal x : {0.0, 0.4, 3.3, width * 0.37 + 0.6, width * 0.62 + 0.15, width - 0.2}) {
            hover(x);
            const QString time = minutesAndSeconds(secondsAtX(seek, x));
            QVERIFY2(timeBox.shown() == time, qPrintable(QStringLiteral("at %1, %2: %3, not %4")
                                                             .arg(dpr).arg(x).arg(timeBox.shown(), time)));
            if (x < 1) {
                QCOMPARE(time, QStringLiteral("0:00"));
            } else if (x > width - 1) {
                QCOMPARE(time, QStringLiteral("3:20"));
            }
            // It is placed from the pointer; seekHoverShowsTheTimeUnderThePointer
            // checks where a window draws it.
            QVERIFY2(fuzzyEqual(timeBox.box->property("anchorX").toReal(), x),
                     qPrintable(QStringLiteral("at %1, %2: centred on %3")
                                    .arg(dpr).arg(x).arg(timeBox.box->property("anchorX").toReal())));
            QCOMPARE(recordedValues(root.data(), "seeks").size(), clicks);

            // A click there seeks to that time.
            const QPointF scene = seek->mapToScene(QPointF(x, y));
            sendMouse(&window, QEvent::MouseButtonPress, scene, Qt::LeftButton, Qt::LeftButton);
            sendMouse(&window, QEvent::MouseButtonRelease, scene, Qt::LeftButton, Qt::NoButton);
            const QList<double> seeks = recordedValues(root.data(), "seeks").mid(clicks);
            QVERIFY2(seeks.size() == 1 && minutesAndSeconds(seeks.constFirst()) == time,
                     qPrintable(QStringLiteral("at %1, %2: shows %3, seeks %4")
                                    .arg(dpr).arg(x).arg(time, describeValues(seeks))));
            ++clicks;
        }

        // A pointer's moves within a device pixel move the box by none, and
        // its text changes only with the second it shows.
        hover(width * 0.5);
        QSignalSpy moves(timeBox.box, SIGNAL(placedXChanged()));
        QSignalSpy texts(timeBox.box, SIGNAL(shownTextChanged()));
        QSet<QString> times{timeBox.shown()};
        constexpr int kSteps = 100;
        constexpr qreal kStep = 0.05;
        for (int step = 1; step <= kSteps; ++step) {
            const qreal x = width * 0.5 + step * kStep;
            hover(x);
            times.insert(minutesAndSeconds(secondsAtX(seek, x)));
            QCOMPARE(timeBox.shown(), minutesAndSeconds(secondsAtX(seek, x)));
        }
        // A new text can change the box's width, and so its position.
        const int devicePixels = static_cast<int>(std::ceil(kSteps * kStep * dpr));
        QVERIFY2(moves.size() >= devicePixels - 1 && moves.size() <= devicePixels + 1 + texts.size(),
                 qPrintable(QStringLiteral("at %1: %2 moves over %3 device pixels")
                                .arg(dpr).arg(moves.size()).arg(devicePixels)));
        QCOMPARE(texts.size(), times.size() - 1);

        // The pointer leaves the bar.
        hover(width + 5);
        QCOMPARE(timeBox.shown(), QString());
    }
}

void AccessibilityTest::seekGesturesShowTheTimeOverThePlayhead() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    const qreal dpr = window->effectiveDevicePixelRatio();
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *line = visibleItemNamed(window, QStringLiteral("seekPlayheadLine"));
    SeekTimeBox timeBox;
    QVERIFY(seek != nullptr && line != nullptr && timeBox.find(window));
    const qreal y = seek->height() / 2;
    const auto lineCentre = [&]() { return line->mapToItem(seek, QPointF(line->width() / 2, 0)).x(); };

    // A drag past the bar's end shows the time over the playhead, at the
    // end, not over the pointer.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                      seek->mapToScene(QPointF(seek->width() * 0.5, y)).toPoint());
    QTest::mouseMove(window, seek->mapToScene(QPointF(seek->width() * 0.7, y)).toPoint());
    QTest::mouseMove(window, seek->mapToScene(QPointF(seek->width() + 30, y)).toPoint());
    QVERIFY(seek->property("adjusting").toBool());
    QCOMPARE(timeBox.shown(), QStringLiteral("3:20"));
    QString placementError = timeBoxPlacementError(seek, timeBox, lineCentre(), dpr);
    QVERIFY2(placementError.isEmpty(), qPrintable(placementError));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                        seek->mapToScene(QPointF(seek->width() + 30, y)).toPoint());
    QVERIFY2(sameValues(recordedValues(root, "seeks"), {200.0}),
             qPrintable(describeValues(recordedValues(root, "seeks"))));
    // Qt delivers no hover during the drag, and brings it up to date at the
    // next frame, before drawing it.
    QTRY_COMPARE(timeBox.shown(), QString());

    // After a drag that ends on the bar, the box shows the time under the
    // pointer where the drag ended, not where it began.
    const QPoint dragEnd = seek->mapToScene(QPointF(seek->width() * 0.7, y)).toPoint();
    const qreal dragEndX = seek->mapFromScene(QPointF(dragEnd)).x();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                      seek->mapToScene(QPointF(seek->width() * 0.3, y)).toPoint());
    QTest::mouseMove(window, seek->mapToScene(QPointF(seek->width() * 0.5, y)).toPoint());
    QTest::mouseMove(window, dragEnd);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dragEnd);
    QCOMPARE(recordedValues(root, "seeks").size(), 2);
    const QString dragEndTime = minutesAndSeconds(secondsAtX(seek, dragEndX));
    QCOMPARE(minutesAndSeconds(recordedValues(root, "seeks").constLast()), dragEndTime);
    QTRY_COMPARE(timeBox.shown(), dragEndTime);
    placementError = timeBoxPlacementError(seek, timeBox, dragEndX, dpr);
    QVERIFY2(placementError.isEmpty(), qPrintable(placementError));

    // Held arrow keys show their time over the playhead while the mouse
    // hovers elsewhere on the bar; the hover's returns when they end.
    QVERIFY(tabTo(window, seek));
    const QPoint hovered = seek->mapToScene(QPointF(seek->width() * 0.8, y)).toPoint();
    QTest::mouseMove(window, hovered);
    const QString hoverTime = minutesAndSeconds(secondsAtX(seek, seek->mapFromScene(QPointF(hovered)).x()));
    QCOMPARE(timeBox.shown(), hoverTime);
    sendKey(window, QEvent::KeyPress, Qt::Key_Right, false);
    QVERIFY(seek->property("adjusting").toBool());
    QCOMPARE(timeBox.shown(), QStringLiteral("0:55"));
    placementError = timeBoxPlacementError(seek, timeBox, lineCentre(), dpr);
    QVERIFY2(placementError.isEmpty(), qPrintable(placementError));
    sendKey(window, QEvent::KeyRelease, Qt::Key_Right, false);
    QVERIFY(!seek->property("adjusting").toBool());
    QVERIFY2(sameValues(recordedValues(root, "seeks").mid(2), {55.0}),
             qPrintable(describeValues(recordedValues(root, "seeks"))));
    QCOMPARE(timeBox.shown(), hoverTime);
}

void AccessibilityTest::seekHoverShowsNothingWithoutATrack() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    auto *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    SeekTimeBox timeBox;
    QVERIFY(seek != nullptr && bridge != nullptr && timeBox.find(window));
    const qreal y = seek->height() / 2;
    const QPoint middle = seek->mapToScene(QPointF(seek->width() * 0.5, y)).toPoint();
    const QPoint later = seek->mapToScene(QPointF(seek->width() * 0.6, y)).toPoint();
    const QPoint away = centerOf(qvariant_cast<QQuickItem *>(root->property("startButton")));

    // No known duration.
    bridge->setProperty("durationSeconds", 0.0);
    QTest::mouseMove(window, middle);
    QTest::mouseMove(window, later);
    QCOMPARE(timeBox.shown(), QString());
    QCOMPARE(timeBox.hiddenText(), QString());
    QTest::mouseMove(window, away);
    bridge->setProperty("durationSeconds", 200.0);
    QTest::mouseMove(window, middle);
    QVERIFY(!timeBox.shown().isEmpty());

    // Stopping hides it, a stopped track's duration shows nothing, and the
    // hover that ended while stopped does not come back with playback.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QCOMPARE(timeBox.shown(), QString());
    QTest::mouseMove(window, later);
    QCOMPARE(timeBox.shown(), QString());
    QTest::mouseMove(window, away);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QCOMPARE(timeBox.shown(), QString());
    QTest::mouseMove(window, later);
    QVERIFY(!timeBox.shown().isEmpty());
    QVERIFY(recordedValues(root, "seeks").isEmpty());
}

namespace {

// Steps `animations` a frame at a time until `done`, for up to five seconds
// of them. A millisecond of real time passes with each step, for what runs
// on it: a hover's delay, and the frames that hand Qt Quick the pointer.
bool stepUntil(SteppedAnimationDriver &animations, const std::function<bool()> &done) {
    for (int elapsed = 0; !done(); elapsed += 16) {
        if (elapsed >= 5000) {
            return false;
        }
        (void)animations.step(16);
        QTest::qWait(1);
    }
    return true;
}

// The seek bar's time in each frame of its fade out.
struct SeekTimeFade {
    // The frames the box was drawn in, part faded.
    int fadingFrames = 0;
    // Why it changed while it faded out, or an empty text.
    QString error;
};

// Hides the seek bar's time, shown in full, by `hide`, and follows its fade
// out to its end, a frame of `animations` at a time: it fades where it was,
// with the time it showed. The hover line, when given, fades with it, at its
// opacity in every frame and where it was, if `lineFades`; otherwise it
// shows in none (it went at once while the time faded).
SeekTimeFade followSeekTimeFade(SteppedAnimationDriver &animations, const SeekTimeBox &box,
                                const std::function<void()> &hide, QQuickItem *hoverLine = nullptr,
                                bool lineFades = false) {
    SeekTimeFade fade;
    auto *content = box.box->property("contentItem").value<QQuickItem *>();
    QQuickItem *popup = content != nullptr ? content->parentItem() : nullptr;
    // A hover's time shows after a moment.
    if (popup == nullptr
        || !stepUntil(animations, [&]() { return popup->isVisible() && popup->opacity() >= 1.0; })) {
        fade.error = QStringLiteral("not shown in full");
        return fade;
    }
    const qreal x = box.x();
    const QString text = box.box->property("text").toString();
    const QString drawnText = content->property("text").toString();
    QSignalSpy moves(box.box, SIGNAL(xChanged()));
    QSignalSpy texts(box.box, SIGNAL(textChanged()));
    QStringList errors;
    const qreal lineX = hoverLine != nullptr ? hoverLine->x() : 0.0;
    if (hoverLine != nullptr && lineFades && !(hoverLine->isVisible() && hoverLine->opacity() >= 1.0)) {
        errors.push_back(QStringLiteral("no hover line shown in full"));
    }
    const auto lineOpacity = [&]() {
        return hoverLine->isVisible() ? hoverLine->opacity() : 0.0;
    };
    const auto check = [&]() {
        if (!popup->isVisible()) {
            return;
        }
        if (hoverLine != nullptr) {
            const qreal expected = lineFades ? popup->opacity() : 0.0;
            if (std::abs(lineOpacity() - expected) > 1e-9 || (lineFades && hoverLine->x() != lineX)) {
                errors.push_back(QStringLiteral("hover line at opacity %1 at %2, the time at %3")
                                     .arg(lineOpacity())
                                     .arg(hoverLine->x())
                                     .arg(popup->opacity()));
            }
        }
        if (popup->opacity() > 0.0 && popup->opacity() < 1.0) {
            ++fade.fadingFrames;
        }
        if (box.x() != x || box.box->property("text").toString() != text
            || content->property("text").toString() != drawnText) {
            errors.push_back(QStringLiteral("%1 at %2 at opacity %3")
                                 .arg(content->property("text").toString())
                                 .arg(box.x())
                                 .arg(popup->opacity()));
        }
    };
    hide();
    check();
    const bool ended = stepUntil(animations, [&]() {
        check();
        return !popup->isVisible();
    });
    if (!ended) {
        errors.push_back(QStringLiteral("did not end"));
    }
    if (hoverLine != nullptr && hoverLine->isVisible()) {
        errors.push_back(QStringLiteral("hover line still shown at %1").arg(hoverLine->opacity()));
    }
    if (!box.shown().isEmpty()) {
        errors.push_back(QStringLiteral("still shows %1").arg(box.shown()));
    }
    // Any move or new text, even one that no frame drew.
    if (!moves.isEmpty() || !texts.isEmpty()) {
        errors.push_back(QStringLiteral("%1 moves and %2 new texts, now %3 at %4, from %5 at %6")
                             .arg(moves.size())
                             .arg(texts.size())
                             .arg(box.box->property("text").toString())
                             .arg(box.x())
                             .arg(text)
                             .arg(x));
    }
    fade.error = errors.join(QStringLiteral("; "));
    return fade;
}

} // namespace

void AccessibilityTest::seekTimeFadesOutWhereItWas() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QObject *root = harness.root.data();
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    const qreal dpr = window->effectiveDevicePixelRatio();
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *line = visibleItemNamed(window, QStringLiteral("seekPlayheadLine"));
    const QList<QQuickItem *> hoverLines = itemsNamed(window->contentItem(), QStringLiteral("seekHoverLine"));
    QQuickItem *hoverLine = hoverLines.size() == 1 ? hoverLines.first() : nullptr;
    auto *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    SeekTimeBox timeBox;
    QVERIFY(seek != nullptr && line != nullptr && hoverLine != nullptr && bridge != nullptr
            && timeBox.find(window));
    // The app's style fades a tooltip out.
    QVERIFY(timeBox.box->property("exit").value<QObject *>() != nullptr);
    // The fades, a frame at a time.
    SteppedAnimationDriver animations;
    const qreal y = seek->height() / 2;
    const QPoint away = centerOf(qvariant_cast<QQuickItem *>(root->property("startButton")));
    const std::optional<SeekPoint> middle = pointBetweenSteps(seek, 0.5);
    const std::optional<SeekPoint> later = pointBetweenSteps(seek, 0.8);
    QVERIFY(middle.has_value() && later.has_value());
    const qreal middleX = seek->mapFromScene(QPointF(middle->scene)).x();
    const qreal laterX = seek->mapFromScene(QPointF(later->scene)).x();
    const QString middleTime = minutesAndSeconds(middle->seconds);
    const QString laterTime = minutesAndSeconds(later->seconds);
    const auto expectShown = [&](const QString &time, qreal anchorX) {
        if (timeBox.shown() != time) {
            return QStringLiteral("shows %1, not %2").arg(timeBox.shown(), time);
        }
        return timeBoxPlacementError(seek, timeBox, anchorX, dpr);
    };
    // The hover's time fades with its line; a gesture's has none.
    const auto expectFade = [&](const std::function<void()> &hide, bool lineFades) {
        const SeekTimeFade fade = followSeekTimeFade(animations, timeBox, hide, hoverLine, lineFades);
        if (!fade.error.isEmpty()) {
            return fade.error;
        }
        return fade.fadingFrames > 0 ? QString() : QStringLiteral("drew no frame part faded");
    };
    QString error;

    // The pointer leaves the bar from its middle: the time there fades out
    // there, never at the bar's start or as its 0:00.
    QTest::mouseMove(window, middle->scene);
    error = expectShown(middleTime, middleX);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    error = expectFade([&]() { QTest::mouseMove(window, away); }, true);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A hover elsewhere shows its own time there, and so does a hover that
    // comes back while the last one fades out.
    QTest::mouseMove(window, later->scene);
    error = expectShown(laterTime, laterX);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QTest::mouseMove(window, away);
    QTest::mouseMove(window, middle->scene);
    error = expectShown(middleTime, middleX);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A drag shows the playhead's time over it, and the hover's comes back
    // at the pointer where it ends.
    const QPoint dragEnd = seek->mapToScene(QPointF(seek->width() * 0.3, y)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, middle->scene);
    QTest::mouseMove(window, seek->mapToScene(QPointF(seek->width() * 0.4, y)).toPoint());
    QTest::mouseMove(window, dragEnd);
    QVERIFY(seek->property("adjusting").toBool());
    error = expectShown(minutesAndSeconds(seek->property("shownSeconds").toDouble()),
                        line->mapToItem(seek, QPointF(line->width() / 2, 0)).x());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dragEnd);
    const qreal dragEndX = seek->mapFromScene(QPointF(dragEnd)).x();
    QTRY_COMPARE(timeBox.shown(), minutesAndSeconds(secondsAtX(seek, dragEndX)));
    error = expectShown(minutesAndSeconds(secondsAtX(seek, dragEndX)), dragEndX);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(recordedValues(root, "seeks").size(), 1);

    // A touch's drag, which has no hover, fades its time out over the
    // playhead.
    QTest::mouseMove(window, away);
    QPointingDevice *touchScreen = QTest::createTouchDevice();
    const QPoint touched = seek->mapToScene(QPointF(seek->width() * 0.2, y)).toPoint();
    QTest::touchEvent(window, touchScreen).press(0, touched, window);
    {
        // Qt Quick hands a touch move on with the next frame it draws, which
        // sending the move can already draw, when the press's is due.
        int frames = 0;
        const FrameSampler counting(window, [&frames]() { ++frames; });
        QTest::touchEvent(window, touchScreen).move(0, touched + QPoint(20, 0), window);
        QTRY_VERIFY(frames > 0);
    }
    error = expectFade([&]() { QTest::touchEvent(window, touchScreen).release(0, touched + QPoint(20, 0), window); },
                       false);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(recordedValues(root, "seeks").size(), 2);

    // Stopping fades the time out where it was; playing again shows it.
    QTest::mouseMove(window, later->scene);
    error = expectFade([&]() { bridge->setProperty("playbackState", QStringLiteral("Stopped")); }, true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    error = expectShown(laterTime, laterX);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A track of another length shows its own time under the pointer; one of
    // no known length fades the last time out where it was.
    bridge->setProperty("durationSeconds", 100.0);
    error = expectShown(minutesAndSeconds(secondsAtX(seek, laterX)), laterX);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(minutesAndSeconds(secondsAtX(seek, laterX)) != laterTime);
    error = expectFade([&]() { bridge->setProperty("durationSeconds", 0.0); }, true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    bridge->setProperty("durationSeconds", 200.0);
    error = expectShown(laterTime, laterX);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(recordedValues(root, "seeks").size(), 2);
}

namespace {

// The transport harness, with what the tests of the seek bar's hover under
// a touch read and send.
struct SeekTouchHarness {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = nullptr;
    QQuickItem *seek = nullptr;
    QObject *hover = nullptr;
    SeekTimeBox timeBox;
    QPointingDevice *touchScreen = QTest::createTouchDevice();
    // On the bar; on the start button, off the bar; and under the
    // transport bar, where nothing takes a press.
    QPoint onBar;
    QPoint offBar;
    QPoint empty;
    std::unique_ptr<QQuickItem> drawn;

    // Loads the harness; an empty text, or why it could not.
    QString load() {
        QString error;
        window = loadTransportHarness(engine, root, &error);
        if (window == nullptr) {
            return error.isEmpty() ? QStringLiteral("the harness did not show") : error;
        }
        seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
        if (seek == nullptr || !timeBox.find(window)) {
            return QStringLiteral("no seek bar or time box");
        }
        hover = seek->findChild<QObject *>(QStringLiteral("seekHover"));
        if (hover == nullptr) {
            return QStringLiteral("no seek hover handler");
        }
        onBar = seek->mapToScene(QPointF(seek->width() * 0.75, seek->height() / 2)).toPoint();
        offBar = centerOf(qvariant_cast<QQuickItem *>(root->property("startButton")));
        empty = QPoint(window->width() / 2, window->height() - 10);
        drawn = std::make_unique<QQuickItem>(window->contentItem());
        // The mouse rests under the transport bar, wherever the test before
        // left it: Qt drops a move to where the cursor already is, which it
        // remembers for the whole program, so one of these two arrives.
        QTest::mouseMove(window, empty + QPoint(1, 0));
        QTest::mouseMove(window, empty);
        return QString();
    }
    // At each frame that has something to draw, as playback's have, Qt
    // hovers what is under a held touch's point with an event that looks
    // like the mouse's (#604). A check made before the next frame would
    // pass or fail by whether one was drawn yet, so the checks follow a
    // frame that had an item to draw.
    bool drawFrame() {
        QSignalSpy frames(window, &QQuickWindow::frameSwapped);
        drawn->setX(drawn->x() + 1);
        return frames.wait();
    }
    bool hovered() const {
        return hover->property("hovered").toBool();
    }
    // The time the bar shows for a pointer at `scene`.
    QString timeAt(QPoint scene) const {
        return minutesAndSeconds(secondsAtX(seek, seek->mapFromScene(QPointF(scene)).x()));
    }
    QTest::QTouchEventSequence touch() {
        return QTest::touchEvent(window, touchScreen);
    }
    // A finger's press on the start button and its release.
    void tap() {
        touch().press(0, offBar, window);
        touch().release(0, offBar, window);
    }
};

} // namespace

void AccessibilityTest::seekTouchShowsNoHoverTime() {
    SeekTouchHarness h;
    const QString loadError = h.load();
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));

    // A touch's drag shows the box as a mouse's does, and its end leaves
    // none, as a touch has no hover.
    h.touch().press(0, h.onBar, h.window);
    QVERIFY(h.seek->property("adjusting").toBool());
    QVERIFY(!h.timeBox.shown().isEmpty());
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    h.touch().move(0, h.onBar + QPoint(5, 0), h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    h.touch().release(0, h.onBar + QPoint(5, 0), h.window);
    QVERIFY(!h.seek->property("adjusting").toBool());
    QCOMPARE(recordedValues(h.root.data(), "seeks").size(), 1);
    QCOMPARE(h.timeBox.shown(), QString());
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());

    // A touch that begins off the bar and moves over it shows nothing.
    h.touch().press(0, h.offBar, h.window);
    h.touch().move(0, (h.offBar + h.onBar) / 2, h.window);
    h.touch().move(0, h.onBar, h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    h.touch().move(0, h.onBar + QPoint(5, 0), h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QVERIFY(!h.seek->property("adjusting").toBool());
    QCOMPARE(h.timeBox.shown(), QString());
    h.touch().release(0, h.onBar + QPoint(5, 0), h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());
    QCOMPARE(recordedValues(h.root.data(), "seeks").size(), 1);

    // The mouse has its hover again once it moves.
    QTest::mouseMove(h.window, h.onBar);
    QVERIFY(h.hovered());
    QCOMPARE(h.timeBox.shown(), h.timeAt(h.onBar));
    QVERIFY(h.drawFrame());
    QCOMPARE(h.timeBox.shown(), h.timeAt(h.onBar));

    // A touch elsewhere ends it, and it stays away until the mouse moves.
    h.touch().press(0, h.offBar, h.window);
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());
    h.touch().release(0, h.offBar, h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());
    QTest::mouseMove(h.window, h.onBar + QPoint(1, 0));
    QVERIFY(h.hovered());
    QVERIFY(!h.timeBox.shown().isEmpty());
    QCOMPARE(recordedValues(h.root.data(), "seeks").size(), 1);
}

void AccessibilityTest::seekTouchShowsNoHoverTimeAfterTheMouseMoved() {
    SeekTouchHarness h;
    const QString loadError = h.load();
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));

    // With a finger held off the bar, the mouse moves, and the finger
    // then slides onto the bar: the window's pointer is the finger again.
    h.touch().press(0, h.offBar, h.window);
    // A move to where the cursor already is would be dropped by Qt, so
    // the mouse goes one pixel along, and the bar's hover is the mouse's
    // again before the finger moves.
    QTest::mouseMove(h.window, h.empty + QPoint(1, 0));
    QVERIFY(h.hover->property("enabled").toBool());
    h.touch().move(0, (h.offBar + h.onBar) / 2, h.window);
    h.touch().move(0, h.onBar, h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());

    // The mouse over the bar has its hover while the finger rests, and
    // loses it when the finger next moves.
    const QPoint moused = h.onBar - QPoint(100, 0);
    QTest::mouseMove(h.window, moused);
    QVERIFY(h.hovered());
    QCOMPARE(h.timeBox.shown(), h.timeAt(moused));
    QVERIFY(h.drawFrame());
    QCOMPARE(h.timeBox.shown(), h.timeAt(moused));
    h.touch().move(0, h.onBar + QPoint(5, 0), h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());
    h.touch().release(0, h.onBar + QPoint(5, 0), h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QVERIFY(recordedValues(h.root.data(), "seeks").isEmpty());
}

void AccessibilityTest::seekTouchShowsNoHoverTimeWhileTheMouseIsInAnotherWindow() {
    SeekTouchHarness h;
    const QString loadError = h.load();
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
    QQuickWindow other;
    other.resize(200, 100);
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&other));

    // A finger rests on the bar, which it slid onto, while the mouse
    // moves, clicks and scrolls in another window, and enters it: this
    // window's pointer is still the finger.
    h.touch().press(0, h.offBar, h.window);
    h.touch().move(0, h.onBar, h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    const QPoint elsewhere(50, 50);
    QTest::mouseMove(&other, elsewhere);
    QTest::mouseClick(&other, Qt::LeftButton, Qt::NoModifier, elsewhere);
    sendWheel(&other, QPointF(elsewhere), QPoint(0, 120));
    QEnterEvent enter(QPointF(elsewhere), QPointF(elsewhere), other.mapToGlobal(QPointF(elsewhere)));
    QCoreApplication::sendEvent(&other, &enter);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());
    h.touch().release(0, h.onBar, h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QVERIFY(recordedValues(h.root.data(), "seeks").isEmpty());
}

void AccessibilityTest::seekHoverReturnsWhenTheMouseEnters() {
    SeekTouchHarness h;
    const QString loadError = h.load();
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));

    // After a touch, the mouse comes into the window over the bar and
    // stops there: Qt hovers what is under it at once, with no move.
    h.tap();
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QEnterEvent enter(QPointF(h.onBar), QPointF(h.onBar), h.window->mapToGlobal(QPointF(h.onBar)));
    QCoreApplication::sendEvent(h.window, &enter);
    QVERIFY(h.hovered());
    QCOMPARE(h.timeBox.shown(), h.timeAt(h.onBar));
    QVERIFY(h.drawFrame());
    QCOMPARE(h.timeBox.shown(), h.timeAt(h.onBar));
    QVERIFY(recordedValues(h.root.data(), "seeks").isEmpty());
}

void AccessibilityTest::seekTouchShowsNoHoverTimeWhenTheMouseScrolls() {
    SeekTouchHarness h;
    const QString loadError = h.load();
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));

    // A wheel over the bar under a held finger leaves the pointer the
    // finger's: Qt takes no position from a wheel, and still hovers what
    // is under the finger.
    h.touch().press(0, h.offBar, h.window);
    h.touch().move(0, h.onBar, h.window);
    sendWheel(h.window, QPointF(h.onBar), QPoint(0, 120));
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    h.touch().release(0, h.onBar, h.window);
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());

    // Nor does a wheel after the touch bring the hover back, as Qt hovers
    // nothing for a wheel; the mouse's next move does.
    sendWheel(h.window, QPointF(h.onBar), QPoint(0, 120));
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QTest::mouseMove(h.window, h.onBar);
    QVERIFY(h.hovered());
}

void AccessibilityTest::seekTouchMadeIntoMouseEventsShowsNoHoverTime() {
    SeekTouchHarness h;
    const QString loadError = h.load();
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));

    // The mouse events that Qt makes of a touch nothing took, which it
    // sends to the window with the touchscreen as their device, and a
    // touchscreen's enter, are still the finger's.
    h.tap();
    const QPointF global = h.window->mapToGlobal(QPointF(h.onBar));
    QMouseEvent move(QEvent::MouseMove, QPointF(h.onBar), global, Qt::NoButton, Qt::LeftButton, Qt::NoModifier,
                     h.touchScreen);
    QCoreApplication::sendEvent(h.window, &move);
    QVERIFY(!h.hovered());
    QEnterEvent enter(QPointF(h.onBar), QPointF(h.onBar), global, h.touchScreen);
    QCoreApplication::sendEvent(h.window, &enter);
    QVERIFY(!h.hovered());
    QVERIFY(h.drawFrame());
    QVERIFY(!h.hovered());
    QCOMPARE(h.timeBox.shown(), QString());

    // The mouse's own move brings its hover back.
    QTest::mouseMove(h.window, h.onBar);
    QVERIFY(h.hovered());
    QCOMPARE(h.timeBox.shown(), h.timeAt(h.onBar));
}

namespace {

// The transport bar under a palette of the given colours, as Kirigami's
// theme and the window's palette give them, and the transport's Header
// colour set's background, `header`.
QByteArray paletteTransportHarnessQml(const QString &window, const QString &base, const QString &alternateBase,
                                      const QString &text, const QString &highlight, const QString &header) {
    return QStringLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components
import "panes" as Panes

ApplicationWindow {
    id: harness
    width: 1000
    height: 200
    visible: true
    palette.window: "%1"
    palette.base: "%2"
    palette.alternateBase: "%3"
    palette.text: "%4"
    palette.windowText: "%4"
    property alias palette0: uiPalette

    Item {
        anchors.fill: parent
        Kirigami.Theme.inherit: false
        Kirigami.Theme.backgroundColor: "%1"
        Kirigami.Theme.textColor: "%4"
        Kirigami.Theme.highlightColor: "%5"

        Components.UiPalette {
            id: uiPalette
            windowRoot: harness
        }

        QtObject {
            id: bridge
            property real durationSeconds: 200
            property string durationText: "03:20"
            property string playbackState: "Playing"
            property real volume: 0.5
            property var waveformPeaksPacked: ""
            property real waveformCoverageSeconds: 0
            property bool waveformComplete: false
        }

        Action { id: previousAction; text: "Previous" }
        Action { id: playAction; text: "Play" }
        Action { id: pauseAction; text: "Pause" }
        Action { id: stopAction; text: "Stop" }
        Action { id: nextAction; text: "Next" }

        Panes.TransportBar {
            width: parent.width
            // The Header colour set's background, as the platform theme
            // gives it to the transport.
            Kirigami.Theme.backgroundColor: "%6"
            uiBridge: bridge
            uiPalette: uiPalette
            previousAction: previousAction
            playAction: playAction
            pauseAction: pauseAction
            stopAction: stopAction
            nextAction: nextAction
            volumeMuted: false
            keyboardSeekStepSeconds: 5
            displayedPositionSeconds: 50
            toggleMutedVolume: function() {}
            setAppVolume: function(value) {}
            normalizedVolumeValue: function(value) { return value }
            seekCommitted: function(value) {}
        }
    }
}
)QML")
        .arg(window, base, alternateBase, text, highlight, header)
        .toUtf8();
}

} // namespace

void AccessibilityTest::seekBarPlayheadContrastsWithTheBar_data() {
    QTest::addColumn<QString>("window");
    QTest::addColumn<QString>("base");
    QTest::addColumn<QString>("alternateBase");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("highlight");
    QTest::addColumn<QString>("header");
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze light") << "#eff0f1" << "#ffffff" << "#f7f7f7" << "#232629" << "#3daee9" << "#dee0e2"
                                  << false;
    QTest::newRow("breeze dark") << "#202326" << "#141618" << "#1d1f22" << "#fcfcfc" << "#3daee9" << "#292c30"
                                 << true;
    QTest::newRow("light, orange accent") << "#f3eee4" << "#fffbf2" << "#ece4d6" << "#1d1b16" << "#f67400"
                                          << "#e6dccb" << false;
    QTest::newRow("dark, purple accent") << "#2a2e32" << "#1b1e20" << "#232629" << "#eff0f1" << "#9b59b6"
                                         << "#31363b" << true;
}

void AccessibilityTest::seekBarPlayheadContrastsWithTheBar() {
    QFETCH(QString, window);
    QFETCH(QString, base);
    QFETCH(QString, alternateBase);
    QFETCH(QString, text);
    QFETCH(QString, highlight);
    QFETCH(QString, header);
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createQml(
        engine, paletteTransportHarnessQml(window, base, alternateBase, text, highlight, header), &errorText));
    auto *quickWindow = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY2(quickWindow != nullptr, qPrintable(errorText));
    QVERIFY(QTest::qWaitForWindowExposed(quickWindow));
    QObject *palette = qvariant_cast<QObject *>(root->property("palette0"));
    QVERIFY(palette != nullptr);
    QCOMPARE(palette->property("themeIsDark").toBool(), dark);
    QCOMPARE(palette->property("uiTextColor").value<QColor>(), QColor::fromString(text));

    QQuickItem *background = visibleItemNamed(quickWindow, QStringLiteral("seekBackground"));
    QQuickItem *unplayed = visibleItemNamed(quickWindow, QStringLiteral("seekUnplayed"));
    QQuickItem *outline = visibleItemNamed(quickWindow, QStringLiteral("seekPlayhead"));
    QQuickItem *line = visibleItemNamed(quickWindow, QStringLiteral("seekPlayheadLine"));
    QVERIFY(background != nullptr && unplayed != nullptr && outline != nullptr && line != nullptr);
    WaveformItem *waveform = nullptr;
    for (QQuickItem *sibling : unplayed->parentItem()->childItems()) {
        waveform = waveform != nullptr ? waveform : qobject_cast<WaveformItem *>(sibling);
    }
    QVERIFY(waveform != nullptr);

    // The line is the amber of `now`, cased in black on a dark theme and in
    // white on a light one.
    const QColor lineColor = line->property("color").value<QColor>();
    const QColor casing = outline->property("color").value<QColor>();
    QCOMPARE(lineColor, palette->property("now").value<QColor>());
    QCOMPARE(casing.rgb() & 0xffffff, dark ? 0x000000u : 0xffffffu);
    QVERIFY(casing.alphaF() > 0.5f && casing.alphaF() < 1.0f);

    // The bar is the transport's chrome, its Header colour;
    // the waveform is drawn on it, letting it show through, and the
    // unplayed part is the chrome over it.
    const QColor chrome = background->property("color").value<QColor>();
    const QColor played = waveform->property("barColor").value<QColor>();
    QObject *transport = nullptr;
    for (QQuickItem *item = unplayed; item != nullptr && transport == nullptr; item = item->parentItem()) {
        transport = item->property("seekUnplayedCover").isValid() ? item : nullptr;
    }
    QVERIFY(transport != nullptr);
    const QColor cover = transport->property("seekUnplayedCover").value<QColor>();
    QCOMPARE(chrome, QColor::fromString(header));
    QCOMPARE(waveform->property("bgColor").value<QColor>().alpha(), 0);
    QCOMPARE(chrome.alphaF(), 1.0f);
    QCOMPARE(played.alphaF(), 1.0f);
    QVERIFY(cover.alphaF() > 0.0f && cover.alphaF() < 1.0f);
    const QColor unplayedWaveform = drawnOver(cover, played);
    QVERIFY2(contrastRatio(unplayedWaveform, chrome) >= 3.0,
             qPrintable(QStringLiteral("unplayed %1 on %2: %3:1")
                            .arg(unplayedWaveform.name(), chrome.name())
                            .arg(contrastRatio(unplayedWaveform, chrome))));

    // The line stands out against its casing over the bar and the
    // waveform, played or not; the line or the casing stands out against
    // the bar and the played part, which the casing is there to separate
    // it from by luminance.
    const QList<std::pair<QString, QColor>> behind{
        {QStringLiteral("bar"), chrome},
        {QStringLiteral("played waveform"), played},
        {QStringLiteral("unplayed waveform"), unplayedWaveform},
    };
    for (const auto &[name, color] : behind) {
        const QColor casingDrawn = drawnOver(casing, color);
        const double lineContrast = contrastRatio(lineColor, color);
        const double casingContrast = contrastRatio(casingDrawn, color);
        QVERIFY2(color == unplayedWaveform || std::max(lineContrast, casingContrast) >= 3.0,
                 qPrintable(QStringLiteral("%1 %2: line %3:1, casing %4:1")
                                .arg(name, color.name()).arg(lineContrast).arg(casingContrast)));
        QVERIFY2(contrastRatio(lineColor, casingDrawn) >= 3.0,
                 qPrintable(QStringLiteral("%1 %2: line on its casing %3:1")
                                .arg(name, color.name()).arg(contrastRatio(lineColor, casingDrawn))));
    }
}

namespace {

// A start far from zero, as a monotonic clock has.
constexpr qint64 kClockStartNs = 5'000'000'000'000LL;
constexpr qint64 kNsPerSecond = 1'000'000'000LL;

// The transport harness on `clock`, with a track of `durationSeconds`.
struct ClockHarness {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window{nullptr};
    QQuickItem *seek{nullptr};
    QQuickItem *line{nullptr};
    QAccessibleInterface *seekInterface{nullptr};

    bool load(PlaybackClock &clock, double durationSeconds, QString *errorText) {
        engine.rootContext()->setContextProperty(QStringLiteral("testClock"), &clock);
        window = loadTransportHarness(engine, root, errorText, clockTransportHarnessQml());
        if (window == nullptr) {
            return false;
        }
        QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
        seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
        line = visibleItemNamed(window, QStringLiteral("seekPlayheadLine"));
        if (bridge == nullptr || seek == nullptr || line == nullptr) {
            *errorText = QStringLiteral("harness items missing");
            return false;
        }
        bridge->setProperty("durationSeconds", durationSeconds);
        seekInterface = QAccessible::queryAccessibleInterface(seek);
        return seekInterface != nullptr;
    }

    QString description() const { return seekInterface->text(QAccessible::Description); }
};

} // namespace

void AccessibilityTest::playbackClockMovesTheSeekBarByPixelsAndItsTextsBySeconds() {
    PlaybackClock clock;
    qint64 now = kClockStartNs;
    clock.setTimeSource([&now]() { return now; });
    ClockHarness harness;
    QString errorText;
    // A 20 s track: the bar moves by about 30 device pixels a second.
    constexpr double kDuration = 20.0;
    QVERIFY2(harness.load(clock, kDuration, &errorText), qPrintable(errorText));
    const double travelPixels = harness.seek->property("travelDevicePixels").toDouble();
    QVERIFY(travelPixels > 300.0);
    const double step = kDuration / travelPixels;
    QCOMPARE(clock.positionStepSeconds(), step);
    const auto pixelOf = [step](double seconds) { return std::floor(seconds / step + 0.5); };

    // The line's place on the bar.
    const auto lineX = [&harness]() {
        return harness.line->mapToItem(harness.line->parentItem()->parentItem(), QPointF()).x();
    };
    QList<double> shown;
    int lineMoves = 0;
    qreal lineAt = 0.0;
    QStringList descriptions;
    connect(&clock, &PlaybackClock::positionChanged, harness.window, [&]() {
        shown.push_back(clock.positionSeconds());
        if (lineX() != lineAt) {
            ++lineMoves;
            lineAt = lineX();
        }
        const QString description = harness.description();
        if (descriptions.isEmpty() || descriptions.last() != description) {
            descriptions.push_back(description);
        }
    });
    QSignalSpy shownTimes(harness.seek, SIGNAL(shownSecondsChanged()));
    QSignalSpy values(harness.seek, SIGNAL(valueChanged()));
    QSignalSpy texts(&clock, &PlaybackClock::positionTextChanged);
    connect(&clock, &PlaybackClock::positionTextChanged, harness.window, [&]() {
        const QString description = harness.description();
        if (descriptions.isEmpty() || descriptions.last() != description) {
            descriptions.push_back(description);
        }
    });

    // Three seconds of playback from 1.1 s, in the clock's own ticks.
    clock.applySample(PlaybackSample{1.1, now, true, kDuration, 1}, true);
    lineAt = lineX();
    shown.clear();
    lineMoves = 0;
    descriptions = QStringList{harness.description()};
    shownTimes.clear();
    values.clear();
    texts.clear();
    const qint64 end = now + 3 * kNsPerSecond;
    int ticks = 0;
    while (clock.nextTickNs().has_value() && *clock.nextTickNs() <= end) {
        now = *clock.nextTickNs();
        clock.tick();
        ++ticks;
    }
    now = end;

    // The position, the bar's time and the playhead moved at every pixel
    // boundary, once, at most a millisecond after it, and only then.
    const double endSeconds = clock.currentPositionSeconds();
    const int pixels = static_cast<int>(pixelOf(endSeconds) - pixelOf(1.1));
    const QString counts = QStringLiteral("%1 boundaries: %2 positions, %3 bar times, %4 playhead moves, %5 ticks")
                               .arg(pixels).arg(shown.size()).arg(shownTimes.count()).arg(lineMoves).arg(ticks);
    QVERIFY2(pixels > 80, qPrintable(counts));
    QVERIFY2(shown.size() == pixels && shownTimes.count() == pixels && lineMoves == pixels, qPrintable(counts));
    for (qsizetype i = 0; i < shown.size(); ++i) {
        const double pixel = pixelOf(1.1) + static_cast<double>(i) + 1.0;
        QCOMPARE(pixelOf(shown[i]), pixel);
        const double late = shown[i] - (pixel - 0.5) * step;
        QVERIFY2(late >= 0.0 && late <= 0.0011, qPrintable(QString::number(late)));
    }
    // The texts and the slider's value, which assistive technology reads,
    // changed at 1.5, 2.5 and 3.5 s, and only then.
    QCOMPARE(texts.count(), 3);
    QCOMPARE(values.count(), 3);
    QCOMPARE(harness.seek->property("value").toDouble(), 4.0);
    QCOMPARE(clock.positionText(), QStringLiteral("00:04"));
    QCOMPARE(descriptions,
             (QStringList{QStringLiteral("0:01 of 0:20"), QStringLiteral("0:02 of 0:20"),
                          QStringLiteral("0:03 of 0:20"), QStringLiteral("0:04 of 0:20")}));
    QVERIFY2(ticks <= pixels + texts.count(), qPrintable(counts));

    // Paused, the clock has no more to show.
    clock.applySample(PlaybackSample{endSeconds, now, false, kDuration, 1}, false);
    QVERIFY(!clock.nextTickNs().has_value());
}

void AccessibilityTest::seekBarDescriptionFollowsTheSecondsBetweenPixels() {
    PlaybackClock clock;
    qint64 now = kClockStartNs;
    clock.setTimeSource([&now]() { return now; });
    ClockHarness harness;
    QString errorText;
    // An hour-long track: a pixel of the bar is seconds long.
    constexpr double kDuration = 3600.0;
    QVERIFY2(harness.load(clock, kDuration, &errorText), qPrintable(errorText));
    const double step = clock.positionStepSeconds();
    QVERIFY2(step > 2.0, qPrintable(QString::number(step)));

    QStringList descriptions;
    const auto record = [&]() {
        const QString description = harness.description();
        if (descriptions.isEmpty() || descriptions.last() != description) {
            descriptions.push_back(description);
        }
    };
    connect(&clock, &PlaybackClock::positionTextChanged, harness.window, record);
    connect(&clock, &PlaybackClock::positionChanged, harness.window, record);
    QSignalSpy shownTimes(harness.seek, SIGNAL(shownSecondsChanged()));
    QSignalSpy values(harness.seek, SIGNAL(valueChanged()));

    // Ten seconds from 100.2 s: the description and the time label take
    // every second, though the bar moves only every few.
    clock.applySample(PlaybackSample{100.2, now, true, kDuration, 1}, true);
    descriptions = QStringList{harness.description()};
    shownTimes.clear();
    values.clear();
    const qint64 end = now + 10 * kNsPerSecond;
    while (clock.nextTickNs().has_value() && *clock.nextTickNs() <= end) {
        now = *clock.nextTickNs();
        clock.tick();
    }
    now = end;
    QStringList expected;
    for (int second = 100; second <= 110; ++second) {
        expected.push_back(QStringLiteral("%1 of 1:00:00").arg(minutesAndSeconds(second)));
    }
    QCOMPARE(descriptions, expected);
    QCOMPARE(clock.positionText(), PlaybackClock::formatSeconds(110));
    QVERIFY2(shownTimes.count() <= static_cast<int>(10.0 / step) + 1, qPrintable(QString::number(shownTimes.count())));
    // The slider's value, which assistive technology reads, takes every
    // second with the description.
    QCOMPARE(values.count(), 10);
    QCOMPARE(harness.seek->property("value").toDouble(), 110.0);

    // A held key's gesture shows its own second, from where playback is.
    QVERIFY(tabTo(harness.window, harness.seek));
    QTest::keyPress(harness.window, Qt::Key_Right);
    QVERIFY(harness.seek->property("adjusting").toBool());
    QCOMPARE(harness.description(),
             QStringLiteral("%1 of 1:00:00").arg(minutesAndSeconds(std::floor(110.2 + 5.0))));
    QTest::keyRelease(harness.window, Qt::Key_Right);
    const QList<double> seeks = recordedValues(harness.root.data(), "seeks");
    QVERIFY2(sameValues(seeks, {115.2}), qPrintable(describeValues(seeks)));
}

void AccessibilityTest::seekBarCatchesUpWithRealPlayback() {
    // The clock's own timer, in real time. A busy machine may wake it late
    // and skip pixels; it must still catch up, change only at a pixel, and
    // stop when playback pauses.
    PlaybackClock clock;
    ClockHarness harness;
    QString errorText;
    constexpr double kDuration = 20.0;
    QVERIFY2(harness.load(clock, kDuration, &errorText), qPrintable(errorText));
    const double step = clock.positionStepSeconds();
    QVERIFY(step > 0.0 && step < 0.1);
    const auto pixelOf = [step](double seconds) { return std::floor(seconds / step + 0.5); };

    int changes = 0;
    int withinAPixel = 0;
    double lastPixel = -1.0;
    connect(&clock, &PlaybackClock::positionChanged, harness.window, [&]() {
        ++changes;
        const double pixel = pixelOf(clock.positionSeconds());
        withinAPixel += pixel == lastPixel ? 1 : 0;
        lastPixel = pixel;
    });
    clock.applySample(PlaybackSample{1.1, monotonicNowNs(), true, kDuration, 1}, true);
    lastPixel = pixelOf(clock.positionSeconds());
    changes = 0;

    QTRY_VERIFY2(changes >= 3, qPrintable(QString::number(changes)));
    // The bar is never more than a pixel and a little behind playback, and
    // the time label shows the second playback is in.
    QTRY_VERIFY(std::abs(harness.seek->property("shownSeconds").toDouble() - clock.currentPositionSeconds())
                <= step + 0.005);
    QTRY_COMPARE(clock.positionText(), PlaybackClock::formatSeconds(clock.currentPositionSeconds()));
    QCOMPARE(withinAPixel, 0);

    // Paused: nothing more changes.
    clock.applySample(PlaybackSample{clock.currentPositionSeconds(), 0, false, kDuration, 1}, false);
    QVERIFY(!clock.nextTickNs().has_value());
    QCOMPARE(harness.seek->property("shownSeconds").toDouble(), clock.positionSeconds());
    const int paused = changes;
    QTest::qWait(200);
    QCOMPARE(changes, paused);
}

void AccessibilityTest::seekBarStepsFromThePositionBetweenItsPixels() {
    PlaybackClock clock;
    qint64 now = 5'000'000'000'000LL;
    clock.setTimeSource([&now]() { return now; });
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testClock"), &clock);
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText, clockTransportHarnessQml());
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(bridge != nullptr && seek != nullptr);
    // An hour-long track: a pixel of the bar is seconds long.
    constexpr double kDuration = 3600.0;
    bridge->setProperty("durationSeconds", kDuration);
    const double step = clock.positionStepSeconds();
    QVERIFY2(step > 2.5, qPrintable(QString::number(step)));
    const auto seeks = [&root]() { return recordedValues(root.data(), "seeks"); };

    // Playing from just past a pixel's boundary, then two seconds on: the
    // bar still shows the start.
    const double start = (std::floor(1000.0 / step) + 0.5) * step + 0.01;
    clock.applySample(PlaybackSample{start, now, true, kDuration, 1}, true);
    QVERIFY(fuzzyEqual(seek->property("shownSeconds").toDouble(), start));
    now += 2'000'000'000LL;
    QVERIFY(fuzzyEqual(clock.currentPositionSeconds(), start + 2.0));
    QVERIFY(fuzzyEqual(seek->property("shownSeconds").toDouble(), start));
    // The slider's value, which assistive technology reads, moves on with
    // the second the description gives, not with the bar.
    while (clock.nextTickNs().has_value() && *clock.nextTickNs() <= now) {
        clock.tick();
    }
    QVERIFY2(clock.positionSecond() > std::lround(start), qPrintable(QString::number(clock.positionSecond())));
    QCOMPARE(seek->property("value").toDouble(), static_cast<double>(clock.positionSecond()));

    // An arrow key steps from where playback is.
    QVERIFY(tabTo(window, seek));
    QTest::keyClick(window, Qt::Key_Right);
    QList<double> sent = seeks();
    QVERIFY2(sameValues(sent, {start + 7.0}), qPrintable(describeValues(sent)));

    // So does the wheel, from where playback is by then.
    now += 1'000'000'000LL;
    seek->setProperty("wheelEnabled", true);
    QVERIFY(sendWheel(window, seek->mapToScene(QPointF(seek->width() / 2, seek->height() / 2)),
                      QPoint(0, 120)));
    sent = seeks();
    QVERIFY2(sameValues(sent, {start + 7.0, start + 8.0}), qPrintable(describeValues(sent)));
}

void AccessibilityTest::transportBarHasNoWarnings() {
    TransportHarness harness;
    QQuickWindow *window = harness.window;
    QVERIFY2(window != nullptr, qPrintable(harness.error));
    qInfo("Qt Quick Controls style: %s", qPrintable(QQuickStyle::name()));
    // Lay the bar out again at other sizes.
    window->resize(640, 160);
    QTRY_COMPARE(window->width(), 640);
    window->resize(1400, 240);
    QTRY_COMPARE(window->width(), 1400);
    QQuickItem *handle = visibleItemNamed(window, QStringLiteral("seekGrabTarget"));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(handle != nullptr && seek != nullptr);
    qInfo("seek slider height %g, grab target height %g", seek->height(), handle->height());
    // cleanup() fails on any warning, such as a binding loop.
}

void AccessibilityTest::transportBarHasNoWarningsUnderTheBasicStyle() {
    // The style is fixed once a control loads, so a child process runs the
    // check above under the Basic style. main() sets the app's style, which
    // would override $QT_QUICK_CONTROLS_STYLE, so it sets the child's.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(childControlsStyleVariable), QStringLiteral("Basic"));
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("transportBarHasNoWarnings")});
    QVERIFY2(child.waitForFinished(120000), "the child test did not finish");
    const QString output = QString::fromLocal8Bit(child.readAll());
    QVERIFY2(output.contains(QStringLiteral("Qt Quick Controls style: Basic")), qPrintable(output));
    QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, qPrintable(output));
}

void AccessibilityTest::checkClicksLeaveSpaceToPlayOrPause(QQuickWindow *window) {
    TransportFixture f;
    QVERIFY(f.load(window));
    QTRY_VERIFY(f.seek->isEnabled());

    // A click on a button leaves the focus where it was, so Space still
    // plays or pauses.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerOf(f.next));
    QTRY_COMPARE(f.nextTriggered->count(), 1);
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(f.playPauseTriggered->count(), 1);
    QCOMPARE(f.nextTriggered->count(), 1);
    QVERIFY(!f.next->hasActiveFocus());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerOf(f.mute));
    QTRY_COMPARE(accessibleText(f.mute, QAccessible::Name), QStringLiteral("Unmute"));
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(f.playPauseTriggered->count(), 2);
    QCOMPARE(accessibleText(f.mute, QAccessible::Name), QStringLiteral("Unmute"));
    QVERIFY(!f.mute->hasActiveFocus());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerOf(f.mute));
    QTRY_COMPARE(accessibleText(f.mute, QAccessible::Name), QStringLiteral("Mute"));

    // So does a click on the sliders, so the arrow keys keep working
    // where they did.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerOf(f.seek));
    QTRY_VERIFY(f.bridge->property("lastSeekSeconds").toDouble() > 50.0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerOf(f.volume));
    QVERIFY(!f.seek->hasActiveFocus());
    QVERIFY(!f.volume->hasActiveFocus());
    QVERIFY(f.library->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(f.playPauseTriggered->count(), 3);

    // Tab still reaches Next, and Space presses it.
    QVERIFY(tabTo(window, f.next));
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(f.nextTriggered->count(), 2);
    QCOMPARE(f.playPauseTriggered->count(), 3);
}

void AccessibilityTest::checkModifiedKeysReachTheShortcutsFromFocusedButtons(QQuickWindow *window) {
    TransportFixture f;
    QVERIFY(f.load(window));

    // Ctrl+Shift+Space stops whichever button has the focus, and the
    // button does nothing.
    QVERIFY(tabTo(window, f.next));
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(f.stopTriggered->count(), 1);
    QCOMPARE(f.nextTriggered->count(), 0);
    QVERIFY(tabTo(window, f.mute));
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(f.stopTriggered->count(), 2);
    QCOMPARE(accessibleText(f.mute, QAccessible::Name), QStringLiteral("Mute"));
    QVERIFY(tabTo(window, f.modeButton));
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(f.stopTriggered->count(), 3);
    QCOMPARE(f.viewer->property("visualizationMode").toInt(), 0);
    QCOMPARE(f.playPauseTriggered->count(), 0);
}

void AccessibilityTest::rowHeightsAndScrollingFollowTheFont() {
    MainWindowFixture main;
    QQuickWindow *window = main.window;
    LibraryTreeModel &libraryModel = main.libraryModel;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(300));
    bridge->setProperty("queueLength", 300);
    libraryModel.setExpandedKeys(
        QStringList{QStringLiteral("artist|Artist A"), QStringLiteral("album|Artist A|Album A")});
    libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree());
    QTRY_COMPARE(libraryModel.rowCount(), 3);
    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    auto *library = window->findChild<QQuickItem *>(QStringLiteral("libraryView"));
    QObject *queueController = window->findChild<QObject *>(QStringLiteral("queueController"));
    QObject *libraryController = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(queue != nullptr && library != nullptr);
    QVERIFY(queueController != nullptr && libraryController != nullptr);
    QObject *tagEditor = nullptr;
    QObject *nowPlaying = window->findChild<QObject *>(QStringLiteral("nowPlayingPanel"));
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->property("tableRowHeight").isValid()) {
            tagEditor = object;
        }
    }
    QVERIFY(tagEditor != nullptr && nowPlaying != nullptr);
    const qreal defaultTitleSize = nowPlaying->property("titlePointSize").toReal();
    QVERIFY(defaultTitleSize > 0);

    const auto check = [&](qreal expectedRowHeight) {
        QCOMPARE(queue->property("rowHeight").toReal(), expectedRowHeight);
        QCOMPARE(library->property("rowHeight").toReal(), expectedRowHeight);
        QCOMPARE(tagEditor->property("tableRowHeight").toReal(), expectedRowHeight);
        QList<QQuickItem *> rows;
        QTRY_VERIFY(!(rows = visibleRows(queue, QStringLiteral("queueRow"))).isEmpty());
        QTRY_COMPARE(rows.first()->height(), expectedRowHeight);
        QTRY_COMPARE(visibleRows(library, QStringLiteral("libraryRow")).size(), 3);
        for (QQuickItem *row : visibleRows(library, QStringLiteral("libraryRow"))) {
            QCOMPARE(row->height(), expectedRowHeight);
        }
        QTRY_COMPARE(queue->property("contentHeight").toReal(), 300 * expectedRowHeight);

        // Paging moves a viewful less one row.
        const qreal pageRows = std::floor(queue->height() / expectedRowHeight) - 1;
        QCOMPARE(callReal(queueController, "pageStep"), pageRows);
        QCOMPARE(callReal(libraryController, "pageStep"),
                 std::floor(library->height() / expectedRowHeight) - 1);

        // A wheel notch scrolls three rows.
        queue->setProperty("contentY", 0.0);
        const QPointF center = queue->mapToScene(QPointF(queue->width() / 2, queue->height() / 2));
        QVERIFY(sendWheel(window, center, QPoint(0, -120)));
        QCOMPARE(queue->property("contentY").toReal(), 3 * expectedRowHeight);

        // Keeping a row in view scrolls to its row.
        queue->setProperty("contentY", 0.0);
        QVERIFY(QMetaObject::invokeMethod(queueController, "_containIndex",
                                          Q_ARG(QVariant, QVariant::fromValue(queue)),
                                          Q_ARG(QVariant, 200)));
        QVERIFY(std::abs(queue->property("contentY").toReal()
                         - (201 * expectedRowHeight - queue->height())) <= 1.0);
    };

    const qreal defaultRowHeight = queue->property("rowHeight").toReal();
    QVERIFY2(defaultRowHeight >= 16 && defaultRowHeight <= 32,
             qPrintable(QString::number(defaultRowHeight)));
    check(defaultRowHeight);

    const int defaultMinimumWidth = window->minimumWidth();
    QFont larger = window->property("font").value<QFont>();
    larger.setPointSizeF((larger.pointSizeF() > 0 ? larger.pointSizeF() : 10.0) * 2);
    QVERIFY(window->setProperty("font", larger));
    QTRY_VERIFY(queue->property("rowHeight").toReal() > defaultRowHeight * 1.6);
    // The window keeps room for the wider columns.
    QTRY_VERIFY(window->minimumWidth() > defaultMinimumWidth);
    check(queue->property("rowHeight").toReal());
    // The now-playing title grows with them.
    QTRY_VERIFY(nowPlaying->property("titlePointSize").toReal() > defaultTitleSize * 1.6);
    dropMinimumSizeWarnings();
}

void AccessibilityTest::queueHeaderAndRowsShareTheColumnWidths() {
    MainWindowFixture main;
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(queue != nullptr);
    const QStringList columns{
        QStringLiteral("indicator"), QStringLiteral("order"), QStringLiteral("title"),
        QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("length")};

    // The widths of the columns if the header's and the first row's
    // columns line up; otherwise why not, in `problem`.
    QString problem;
    const auto alignedWidths = [&]() -> QList<qreal> {
        const QList<QQuickItem *> rows = visibleRows(queue, QStringLiteral("queueRow"));
        if (rows.size() != 3) {
            problem = QStringLiteral("%1 rows").arg(rows.size());
            return {};
        }
        QList<qreal> widths;
        for (const QString &column : columns) {
            QQuickItem *header = visibleItemNamed(window, QStringLiteral("queueHeader_") + column);
            const QList<QQuickItem *> cells =
                itemsNamed(rows.first(), QStringLiteral("queueCell_") + column);
            if (header == nullptr || cells.size() != 1) {
                problem = QStringLiteral("no column ") + column;
                return {};
            }
            const qreal headerX = header->mapToScene(QPointF()).x();
            const qreal cellX = cells.first()->mapToScene(QPointF()).x();
            if (std::abs(headerX - cellX) > 0.5
                    || std::abs(header->width() - cells.first()->width()) > 0.5) {
                problem = QStringLiteral("column %1: header at %2, %3 wide; cells at %4, %5 wide")
                    .arg(column).arg(headerX).arg(header->width()).arg(cellX)
                    .arg(cells.first()->width());
                return {};
            }
            widths.push_back(header->width());
        }
        return widths;
    };

    QList<qreal> defaults;
    QTRY_VERIFY2((defaults = alignedWidths()).size() == columns.size(), qPrintable(problem));
    auto *pane = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    QVERIFY(pane != nullptr);
    // By default Artist and Album have a little over a quarter and a little
    // less of the room the text columns have, after the glyph, number and
    // length columns and the gaps, and the title the rest.
    const auto checkShares = [&](const QList<qreal> &widths) {
        const qreal table = pane->width();
        const qreal gap = pane->property("columnGap").toReal();
        QStringList all;
        for (qreal width : widths) {
            all << QString::number(width);
        }
        const QString described = QStringLiteral("%1 of %2").arg(all.join(QLatin1Char(' '))).arg(table);
        const qreal room = pane->property("defaultTextColumnsRoom").toReal();
        QVERIFY2(std::abs(widths[3] - room * pane->property("defaultArtistColumnShare").toReal()) <= 1,
                 qPrintable(described));
        QVERIFY2(std::abs(widths[4] - room * pane->property("defaultAlbumColumnShare").toReal()) <= 1,
                 qPrintable(described));
        const qreal rest = table - widths[0] - widths[1] - pane->property("titleGap").toReal()
            - widths[3] - widths[4] - widths[5] - 3 * gap - pane->property("rightPadding").toReal();
        QVERIFY2(std::abs(widths[2] - rest) <= 1, qPrintable(described));
        QVERIFY2(widths[2] >= pane->property("minimumTitleColumnWidth").toReal(), qPrintable(described));
    };
    checkShares(defaults);

    // A larger font widens the glyph, number and length columns, which stay
    // aligned; Artist and Album keep their share.
    QFont larger = window->property("font").value<QFont>();
    larger.setPointSizeF((larger.pointSizeF() > 0 ? larger.pointSizeF() : 10.0) * 1.5);
    QVERIFY(window->setProperty("font", larger));
    QList<qreal> scaled;
    QTRY_VERIFY2((scaled = alignedWidths()).size() == columns.size()
                     && scaled[1] > defaults[1] * 1.3,
                 qPrintable(problem));
    QVERIFY(scaled[0] > defaults[0] * 1.3);
    QVERIFY(scaled[5] > defaults[5] * 1.3);
    checkShares(scaled);
    dropMinimumSizeWarnings();
}

void AccessibilityTest::checkQueueColumnsResizeByTheirHandles(QQuickWindow *window) {
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(queue != nullptr);
    QSignalSpy kept(bridge, SIGNAL(queueColumnWidthsChanged()));
    QVERIFY(kept.isValid());

    // The width of `column` in the header and in the first row, or -1 if
    // they differ.
    const auto widthOf = [&](const QString &column) -> qreal {
        const QList<QQuickItem *> rows = visibleRows(queue, QStringLiteral("queueRow"));
        QQuickItem *header = visibleItemNamed(window, QStringLiteral("queueHeader_") + column);
        if (rows.isEmpty() || header == nullptr) {
            return -1;
        }
        const QList<QQuickItem *> cells =
            itemsNamed(rows.first(), QStringLiteral("queueCell_") + column);
        if (cells.size() != 1
            || std::abs(header->mapToScene(QPointF()).x() - cells.first()->mapToScene(QPointF()).x())
                > 0.5
            || std::abs(header->width() - cells.first()->width()) > 0.5) {
            return -1;
        }
        return header->width();
    };
    qreal defaultArtist = -1;
    QTRY_VERIFY((defaultArtist = widthOf(QStringLiteral("artist"))) > 0);
    const qreal title = widthOf(QStringLiteral("title"));
    QVERIFY(title > 100);

    QQuickItem *artistHandle = visibleItemNamed(window, QStringLiteral("queueColumnHandle_artist"));
    QQuickItem *albumHandle = visibleItemNamed(window, QStringLiteral("queueColumnHandle_album"));
    QVERIFY(artistHandle != nullptr && albumHandle != nullptr);
    QCOMPARE(accessibleText(artistHandle, QAccessible::Name),
             QStringLiteral("Resize the Artist column"));
    QVERIFY(artistHandle->activeFocusOnTab());

    // A drag to the left widens the column after the handle, in the
    // header and the rows together, and the width is kept once, when the
    // drag ends.
    QPoint at = centerOf(artistHandle);
    QTest::mouseMove(window, at);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at);
    for (int step = 0; step < 6; ++step) {
        at -= QPoint(10, 0);
        QTest::mouseMove(window, at);
        QTRY_COMPARE(widthOf(QStringLiteral("artist")), defaultArtist + 10 * (step + 1));
        QTRY_COMPARE(widthOf(QStringLiteral("title")), title - 10 * (step + 1));
    }
    QCOMPARE(kept.count(), 0);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at);
    QTRY_COMPARE(kept.count(), 1);
    QCOMPARE(bridge->property("queueColumnWidths").toMap(),
             (QVariantMap{{QStringLiteral("artist"), int(defaultArtist) + 60}}));
    QCOMPARE(widthOf(QStringLiteral("artist")), defaultArtist + 60);

    // The title keeps its minimum however far the handle goes.
    auto *pane = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    QVERIFY(pane != nullptr);
    at = centerOf(artistHandle);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at);
    QTest::mouseMove(window, at - QPoint(20, 0));
    QTest::mouseMove(window, QPoint(2, at.y()));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, QPoint(2, at.y()));
    QTRY_COMPARE(kept.count(), 2);
    QTRY_VERIFY(std::abs(widthOf(QStringLiteral("title"))
                         - pane->property("minimumTitleColumnWidth").toReal()) <= 1);

    // A double-click restores the default width.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, centerOf(artistHandle));
    QTRY_COMPARE(widthOf(QStringLiteral("artist")), defaultArtist);
    QVERIFY(bridge->property("queueColumnWidths").toMap().isEmpty());
    QCOMPARE(kept.count(), 3);

    // Left and Right resize the focused column, and Home restores it.
    const qreal defaultAlbum = widthOf(QStringLiteral("album"));
    albumHandle->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(albumHandle->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_VERIFY(widthOf(QStringLiteral("album")) > defaultAlbum + 2);
    QCOMPARE(kept.count(), 5);
    const qreal widened = widthOf(QStringLiteral("album"));
    QCOMPARE(bridge->property("queueColumnWidths").toMap().value(QStringLiteral("album")).toInt(),
             int(widened));
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_VERIFY(widthOf(QStringLiteral("album")) < widened);
    QTest::keyClick(window, Qt::Key_Home);
    QTRY_COMPARE(widthOf(QStringLiteral("album")), defaultAlbum);
    QVERIFY(bridge->property("queueColumnWidths").toMap().isEmpty());
}

void AccessibilityTest::checkACompressedQueueColumnResizesFromItsPreferredWidth(QQuickWindow *window) {
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    auto *pane = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    QVERIFY(pane != nullptr);
    const auto shown = [&](const char *column) {
        QQuickItem *header =
            visibleItemNamed(window, QStringLiteral("queueHeader_") + QLatin1String(column));
        return header != nullptr ? header->width() : -1.0;
    };
    const auto handle = [&](const char *column) {
        return visibleItemNamed(window, QStringLiteral("queueColumnHandle_") + QLatin1String(column));
    };
    // Where the handle, the column's left edge, is.
    const auto edge = [&](const char *column) {
        QQuickItem *item = handle(column);
        return item != nullptr ? item->mapToScene(QPointF(item->width() / 2, 0)).x() : -1.0;
    };
    // The pane's widths, which the header's layout follows.
    const auto paneWidth = [&](const char *property) { return pane->property(property).toReal(); };
    const auto saved = [&]() { return bridge->property("queueColumnWidths").toMap(); };
    const auto setWidth = [&](const char *column, int width) {
        return QMetaObject::invokeMethod(bridge, "setQueueColumnWidth",
                                         Q_ARG(QVariant, QString::fromLatin1(column)),
                                         Q_ARG(QVariant, width));
    };
    const qreal minimumTitle = pane->property("minimumTitleColumnWidth").toReal();
    const auto titleAtItsMinimum = [&]() { return std::abs(shown("title") - minimumTitle) <= 1; };
    QSignalSpy kept(bridge, SIGNAL(queueColumnWidthsChanged()));
    QVERIFY(kept.isValid());
    // Whether the header has been laid out with the pane's widths.
    const auto settled = [&]() {
        return shown("artist") == paneWidth("artistColumnWidth")
            && shown("album") == paneWidth("albumColumnWidth")
            && shown("length") == paneWidth("lengthColumnWidth");
    };
    // Artist wider than the queue has room for: the columns give way.
    const auto compress = [&]() {
        return setWidth("artist", 2000) && setWidth("album", 0) && setWidth("length", 0);
    };

    QVERIFY(compress());
    QTRY_VERIFY(shown("artist") > 0 && shown("artist") < 2000);
    QTRY_VERIFY(titleAtItsMinimum());
    kept.clear();

    // A press and release on a handle changes nothing.
    const qreal artist = paneWidth("artistColumnWidth");
    const qreal album = paneWidth("albumColumnWidth");
    QPoint at = centerOf(handle("artist"));
    QTest::mouseMove(window, at);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at);
    QCOMPARE(paneWidth("artistColumnWidth"), artist);
    QCOMPARE(paneWidth("albumColumnWidth"), album);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at);
    QCOMPARE(paneWidth("artistColumnWidth"), artist);
    QCOMPARE(paneWidth("albumColumnWidth"), album);
    QCOMPARE(kept.count(), 0);
    QCOMPARE(saved().value(QStringLiteral("artist")).toInt(), 2000);

    // Drags `column`'s handle `dx` and checks that it moved `moved` with the
    // pointer, while dragging and once the width is kept.
    const auto drag = [&](const char *column, int dx, int moved) {
        QVERIFY(compress());
        QTRY_VERIFY(settled() && titleAtItsMinimum());
        const qreal from = edge(column);
        QPoint pointer = centerOf(handle(column));
        QTest::mouseMove(window, pointer);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, pointer);
        for (int step = 1; step <= 5; ++step) {
            QTest::mouseMove(window, pointer + QPoint(dx * step / 5, 0));
        }
        QTRY_VERIFY2(std::abs(edge(column) - (from + moved)) <= 1,
                     qPrintable(QStringLiteral("%1's handle %2 from %3 after %4")
                                    .arg(QLatin1String(column))
                                    .arg(edge(column) - from)
                                    .arg(from)
                                    .arg(dx)));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, pointer + QPoint(dx, 0));
        QTRY_VERIFY2(std::abs(edge(column) - (from + moved)) <= 1,
                     qPrintable(QStringLiteral("%1's kept handle %2 from %3")
                                    .arg(QLatin1String(column))
                                    .arg(edge(column) - from)
                                    .arg(from)));
    };

    // Album's edge follows the pointer both ways: to the left it takes from
    // Artist, which is giving way, and to the right it gives Artist room.
    drag("album", -30, -30);
    QVERIFY(saved().value(QStringLiteral("album")).toInt() > 0);
    drag("album", 20, 20);
    // Artist's edge moves right, which gives the title room...
    drag("artist", 20, 20);
    QVERIFY(saved().value(QStringLiteral("artist")).toInt() < 2000);
    QVERIFY(shown("title") > minimumTitle + 10);
    // ...but not left, where the title is at its minimum: nothing changes.
    kept.clear();
    drag("artist", -30, 0);
    QCOMPARE(saved().value(QStringLiteral("artist")).toInt(), 2000);
    QCOMPARE(kept.count(), 3);
    QVERIFY(titleAtItsMinimum());

    // Left moves a focused handle by one step, and Right back.
    QVERIFY(compress());
    QTRY_VERIFY(settled() && titleAtItsMinimum());
    const qreal step = pane->property("keyboardResizeStep").toReal();
    QVERIFY(step >= 1);
    QQuickItem *albumHandle = handle("album");
    albumHandle->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(albumHandle->hasActiveFocus());
    const qreal from = edge("album");
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_VERIFY2(std::abs(edge("album") - (from - step)) <= 1,
                 qPrintable(QString::number(edge("album") - from)));
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_VERIFY2(std::abs(edge("album") - from) <= 1,
                 qPrintable(QString::number(edge("album") - from)));

    // However far it goes, the preferred width stays within what the
    // settings keep, and the columns after the title keep their minimums.
    for (int press = 0; press < 80; ++press) {
        QTest::keyClick(window, Qt::Key_Left);
    }
    const int widest = saved().value(QStringLiteral("album")).toInt();
    QVERIFY2(widest > 0 && widest <= 4096, qPrintable(QString::number(widest)));
    QVERIFY(shown("artist") >= pane->property("minimumResizedColumnWidth").toReal() - 1);
    QVERIFY(titleAtItsMinimum());
}

void AccessibilityTest::checkQueueColumnTextStaysInsideItsColumnAtTheMinimum(QQuickWindow *window) {
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVariantList rows;
    for (int i = 0; i < 3; ++i) {
        rows.push_back(QVariantMap{
            {QStringLiteral("title"), QStringLiteral("A long title that does not fit %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("A long artist name %1").arg(i)},
            {QStringLiteral("album"), QStringLiteral("A long album name")},
            {QStringLiteral("lengthText"), QStringLiteral("2:34:56")},
        });
    }
    bridge->setProperty("queueRows", rows);
    bridge->setProperty("queueLength", 3);
    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(queue != nullptr);
    // As narrow as they go.
    for (const char *column : {"artist", "album", "length"}) {
        QVERIFY(QMetaObject::invokeMethod(bridge, "setQueueColumnWidth",
                                          Q_ARG(QVariant, QString::fromLatin1(column)),
                                          Q_ARG(QVariant, 16)));
    }
    const QStringList columns{
        QStringLiteral("indicator"), QStringLiteral("order"), QStringLiteral("title"),
        QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("length")};
    QList<QQuickItem *> shownRows;
    QTRY_VERIFY((shownRows = visibleRows(queue, QStringLiteral("queueRow"))).size() == 3);
    QQuickItem *albumHeader = visibleItemNamed(window, QStringLiteral("queueHeader_album"));
    QVERIFY(albumHeader != nullptr);
    QTRY_VERIFY(albumHeader->width() < 50);

    // Each label's text ends inside its column, before the next one starts.
    const auto check = [&](const QList<QQuickItem *> &labels) {
        for (int i = 0; i < labels.size(); ++i) {
            QQuickItem *label = labels.at(i);
            const qreal left = label->mapToScene(QPointF()).x();
            const qreal textRight = left + label->property("contentWidth").toReal();
            QVERIFY2(textRight <= left + label->width() + 0.5,
                     qPrintable(QStringLiteral("%1: text %2 wide in %3")
                                    .arg(label->objectName())
                                    .arg(label->property("contentWidth").toReal())
                                    .arg(label->width())));
            if (i + 1 < labels.size()) {
                QVERIFY2(textRight <= labels.at(i + 1)->mapToScene(QPointF()).x() + 0.5,
                         qPrintable(label->objectName()));
            }
        }
    };
    QList<QQuickItem *> headers;
    QList<QQuickItem *> cells;
    for (const QString &column : columns) {
        headers.push_back(visibleItemNamed(window, QStringLiteral("queueHeader_") + column));
        const QList<QQuickItem *> found =
            itemsNamed(shownRows.first(), QStringLiteral("queueCell_") + column);
        QCOMPARE(found.size(), 1);
        cells.push_back(found.first());
        QVERIFY(headers.last() != nullptr);
    }
    check(headers);
    check(cells);
    // The longest length and its heading still fit.
    QVERIFY(!cells.last()->property("truncated").toBool());
    QVERIFY(!headers.last()->property("truncated").toBool());
    QVERIFY(cells.at(3)->property("truncated").toBool());
}

// A light theme's window with the transport bar and the tag editor.
static QByteArray lightHarnessQml() {
    return QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components
import "dialogs" as Dialogs
import "panes" as Panes

ApplicationWindow {
    id: harness
    width: 1200
    height: 800
    visible: true
    // A light theme with colours no hard-coded value matches.
    palette.window: "#f3eee4"
    palette.base: "#fffbf2"
    palette.alternateBase: "#ece4d6"
    palette.text: "#1d1b16"
    palette.windowText: "#1d1b16"
    property alias palette0: uiPalette
    property alias tagEditor: tagEditor

    Components.UiPalette {
        id: uiPalette
        windowRoot: harness
    }

    QtObject {
        id: bridge
        property real durationSeconds: 200
        property string durationText: "03:20"
        property string playbackState: "Playing"
        property real volume: 0.5
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 0
        property bool waveformComplete: false
    }

    Action { id: previousAction; text: "Previous" }
    Action { id: playAction; text: "Play" }
    Action { id: pauseAction; text: "Pause" }
    Action { id: stopAction; text: "Stop" }
    Action { id: nextAction; text: "Next" }

    Panes.TransportBar {
        width: parent.width
        // The theme's Header colour, a step darker than its window colour.
        Kirigami.Theme.backgroundColor: "#e6dccb"
        uiBridge: bridge
        uiPalette: uiPalette
        previousAction: previousAction
        playAction: playAction
        pauseAction: pauseAction
        stopAction: stopAction
        nextAction: nextAction
        volumeMuted: false
        keyboardSeekStepSeconds: 5
        displayedPositionSeconds: 50
        toggleMutedVolume: function() {}
        setAppVolume: function(value) {}
        normalizedVolumeValue: function(value) { return value }
        seekCommitted: function(value) {}
    }

    ListModel {
        id: tableModel
        ListElement {
            fileName: "a.flac"; directory: "/music"; formatKind: "FLAC"; title: "A"
            artist: "X"; album: "Y"; genre: ""; year: ""; trackNo: "1"; discNo: ""
            dirty: false; selected: true; errorText: ""; renameFailed: false; extraValues: ""; changedFields: ""
        }
        ListElement {
            fileName: "b.flac"; directory: "/music"; formatKind: "FLAC"; title: "B"
            artist: "X"; album: "Y"; genre: ""; year: ""; trackNo: "2"; discNo: ""
            dirty: false; selected: false; errorText: ""; renameFailed: false; extraValues: ""; changedFields: ""
        }
        ListElement {
            fileName: "c.flac"; directory: "/music"; formatKind: "FLAC"; title: "C"
            artist: "X"; album: "Y"; genre: ""; year: ""; trackNo: "3"; discNo: ""
            dirty: false; selected: false; errorText: ""; renameFailed: false; extraValues: ""; changedFields: ""
        }
    }

    QtObject {
        id: tagEditorApi
        property bool open: false
        property bool loading: false
        property bool saving: false
        property bool dirty: false
        property string statusText: "Saved"
        property string statusDetails: ""
        property bool statusIsError: false
        property int selectedCount: 1
        property var operationFiles: []
        property var tableModel: tableModel
        signal selectionChanged()
        signal bulkSummaryChanged()
        function close() {}
        function reload() {}
        function save() { return true }
        function renameSelectedFiles() {}
        function setSelectedRows(rows) {}
        function selectedRows() { return [0] }
        function selectRange(first, last) {}
        function toggleRowSelected(row) {}
        function selectAllRows() {}
        function loadedPaths() { return [] }
        function bulkValue(field) { return "" }
        function bulkValueMixed(field) { return false }
        function extraValuesNote(field) { return "" }
        function bulkFieldChanged(field) { return field === "genre" }
        function bulkOriginalValue(field) { return "Classical" }
        function bulkOriginalValueMixed(field) { return false }
        function revertBulkField(field) {}
        function fieldChanged(field) { return field === "genre" }
        function revertField(field) {}
        function applyBulkFieldToRows(rows, field, value) {}
        function applyEnglishTitleCase(field) {}
        function applyFinnishCapitalize(field) {}
        function applyGenreCapitalize() {}
        function autoNumber(startingTrack, startingDisc, writeDiscNumbers, writeTotals, resetOnFolder, resetOnDiscChange) {}
    }

    Dialogs.TagEditorDialog {
        id: tagEditor
        tagEditorApi: tagEditorApi
        uiPalette: uiPalette
        windowRoot: harness
    }
}
)QML");
}

void AccessibilityTest::lightPaletteLeavesNoFixedColours() {
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createQml(engine, lightHarnessQml(), &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *palette = qvariant_cast<QObject *>(root->property("palette0"));
    QVERIFY(palette != nullptr);
    QVERIFY(!palette->property("themeIsDark").toBool());
    const auto paletteColor = [palette](const char *name) {
        return palette->property(name).value<QColor>();
    };
    const auto sameRgb = [](const QColor &a, const QColor &b) {
        return a.red() == b.red() && a.green() == b.green() && a.blue() == b.blue();
    };

    // The seek bar, on the transport's chrome: the theme's Header colour.
    const QColor header(QStringLiteral("#e6dccb"));
    QQuickItem *background = visibleItemNamed(window, QStringLiteral("seekBackground"));
    QVERIFY(background != nullptr);
    QCOMPARE(background->property("color").value<QColor>(), header);
    QQuickItem *unplayed = visibleItemNamed(window, QStringLiteral("seekUnplayed"));
    QVERIFY(unplayed != nullptr);
    QObject *transport = nullptr;
    for (QQuickItem *item = unplayed; item != nullptr && transport == nullptr; item = item->parentItem()) {
        transport = item->property("seekUnplayedCover").isValid() ? item : nullptr;
    }
    QVERIFY(transport != nullptr);
    QVERIFY(sameRgb(transport->property("seekUnplayedCover").value<QColor>(), header));
    QQuickItem *playhead = visibleItemNamed(window, QStringLiteral("seekPlayhead"));
    QQuickItem *playheadLine = visibleItemNamed(window, QStringLiteral("seekPlayheadLine"));
    QVERIFY(playhead != nullptr && playheadLine != nullptr);
    QCOMPARE(playheadLine->property("color").value<QColor>(), paletteColor("now"));
    QVERIFY(sameRgb(playhead->property("color").value<QColor>(), QColor(Qt::white)));
    QList<WaveformItem *> waveforms;
    const std::function<void(QQuickItem *)> findWaveforms = [&](QQuickItem *item) {
        if (auto *waveform = qobject_cast<WaveformItem *>(item)) {
            waveforms.push_back(waveform);
        }
        for (QQuickItem *child : item->childItems()) {
            findWaveforms(child);
        }
    };
    findWaveforms(window->contentItem());
    QCOMPARE(waveforms.size(), 1);
    // The chrome shows through the waveform.
    QCOMPARE(waveforms.first()->property("bgColor").value<QColor>().alpha(), 0);
    // The theme's `wave`, mixed 30% toward the text on a light theme.
    const QColor wave = paletteColor("wave");
    const QColor text = paletteColor("text");
    const QColor played = waveforms.first()->property("barColor").value<QColor>();
    QVERIFY2(std::abs(played.redF() - (wave.redF() * 0.7 + text.redF() * 0.3)) < 0.01
                 && std::abs(played.greenF() - (wave.greenF() * 0.7 + text.greenF() * 0.3)) < 0.01
                 && std::abs(played.blueF() - (wave.blueF() * 0.7 + text.blueF() * 0.3)) < 0.01,
             qPrintable(played.name()));
    QQuickItem *mute = visibleItemNamed(window, QStringLiteral("muteButton"));
    QVERIFY(mute != nullptr);
    QCOMPARE(evaluateOn(mute, QStringLiteral("icon.color")).value<QColor>(), paletteColor("text"));

    // The tag editor's rows, header and status, in the light theme's
    // tokens: the selected row is the selection tint, the others are the
    // view, and a changed column's header is the text colour.
    QObject *tagEditor = qvariant_cast<QObject *>(root->property("tagEditor"));
    QVERIFY(QMetaObject::invokeMethod(tagEditor, "open"));
    // Edit Tags has a window of its own.
    auto *tags = qobject_cast<QQuickWindow *>(tagEditor);
    QVERIFY(tags != nullptr && QTest::qWaitForWindowExposed(tags));
    QList<QQuickItem *> rows;
    QTRY_COMPARE((rows = visibleRows(tags->contentItem(), QStringLiteral("tagEditorRow"))).size(), 3);
    const auto rowTint = [](QQuickItem *row) {
        const QList<QQuickItem *> tints = row->findChildren<QQuickItem *>(QStringLiteral("rowBackgroundTint"));
        return tints.isEmpty() ? nullptr : tints.first();
    };
    QVERIFY(rowTint(rows[0]) != nullptr && rowTint(rows[0])->isVisible());
    QVERIFY(sameRgb(rowTint(rows[0])->property("color").value<QColor>(), paletteColor("selectionInactive"))
            || sameRgb(rowTint(rows[0])->property("color").value<QColor>(), paletteColor("selection")));
    QVERIFY(!rowTint(rows[1])->isVisible());
    QVERIFY(!rowTint(rows[2])->isVisible());
    QQuickItem *genre = visibleItemNamed(tags, QStringLiteral("tagEditorColumn_genre"));
    QQuickItem *title = visibleItemNamed(tags, QStringLiteral("tagEditorColumn_title"));
    QVERIFY(genre != nullptr && title != nullptr);
    const auto columnLabelColor = [](QQuickItem *column) {
        return column->findChildren<QQuickItem *>(QStringLiteral("tagEditorColumnLabel")).first()
            ->property("color").value<QColor>();
    };
    QVERIFY(sameRgb(columnLabelColor(genre), paletteColor("text")));
    QVERIFY(sameRgb(columnLabelColor(title), paletteColor("textSecondary")));
    QQuickItem *status = visibleItemNamed(tags, QStringLiteral("tagEditorStatusLabel"));
    QVERIFY(status != nullptr);
    QVERIFY(sameRgb(status->property("color").value<QColor>(), paletteColor("textSecondary")));
}

namespace {
// Defined with the plot's contrast tests, below.
QColor drawnAt(const QImage &image, const QQuickWindow *window, QQuickItem *item, QPointF point);
} // namespace

// The mode switch's segments tell assistive technology what they are:
// radio buttons, checkable, the chosen one checked (they told of their
// checked state changing but never that they could be checked, and read as
// plain buttons). A click, Space and the press action choose a mode;
// choosing the chosen one again leaves it chosen.
void AccessibilityTest::checkModeSwitchSegmentsAreCheckableRadioButtons(QQuickWindow *window) {
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QQuickItem *spectrogramButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton0"));
    QQuickItem *waveformButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QVERIFY(viewer != nullptr && spectrogramButton != nullptr && waveformButton != nullptr);
    const auto states = [&](QQuickItem *button) {
        QAccessibleInterface *iface = inAccessibleTree(window, button);
        return iface != nullptr ? iface->state() : QAccessible::State();
    };
    // Why the switch does not say `mode` is chosen, or "".
    const auto problem = [&](int mode) -> QString {
        for (QQuickItem *button : {spectrogramButton, waveformButton}) {
            const bool chosen = button == (mode == 0 ? spectrogramButton : waveformButton);
            const QAccessible::State state = states(button);
            if (accessibleRole(button) != QAccessible::RadioButton) {
                return QStringLiteral("%1 is not a radio button").arg(button->objectName());
            }
            if (!state.checkable) {
                return QStringLiteral("%1 is not checkable").arg(button->objectName());
            }
            if (bool(state.checked) != chosen || button->property("checked").toBool() != chosen) {
                return QStringLiteral("%1 checked %2").arg(button->objectName()).arg(bool(state.checked));
            }
        }
        return {};
    };
    QCOMPARE(viewer->property("visualizationMode").toInt(), 0);
    QTRY_VERIFY2(problem(0).isEmpty(), qPrintable(problem(0)));

    // A click on the other one chooses it.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      waveformButton->mapToScene(QPointF(12, 12)).toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QTRY_VERIFY2(problem(1).isEmpty(), qPrintable(problem(1)));
    // A click on the chosen one leaves it chosen.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      waveformButton->mapToScene(QPointF(12, 12)).toPoint());
    QTest::qWait(50);
    QCOMPARE(viewer->property("visualizationMode").toInt(), 1);
    QVERIFY2(problem(1).isEmpty(), qPrintable(problem(1)));
    // The press action, as a screen reader's, chooses the other.
    QAccessibleInterface *iface = inAccessibleTree(window, spectrogramButton);
    QVERIFY(iface != nullptr && iface->actionInterface() != nullptr);
    iface->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QTRY_VERIFY2(problem(0).isEmpty(), qPrintable(problem(0)));
    // The mode set elsewhere, as from the menu: the switch follows.
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
    QTRY_VERIFY2(problem(1).isEmpty(), qPrintable(problem(1)));
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0)));
    QTRY_VERIFY2(problem(0).isEmpty(), qPrintable(problem(0)));
}

void AccessibilityTest::checkModeSwitchIconFollowsTheMode(QQuickWindow *window) {
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QQuickItem *spectrogramButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton0"));
    QQuickItem *waveformButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QVERIFY(viewer != nullptr && spectrogramButton != nullptr && waveformButton != nullptr);
    // Real tool buttons with glyphs, tooltips and accessible names: the
    // spectrogram's a note's traces over a beat's pulses as a 16 px
    // symbolic mask (Breeze's bar chart said statistics), the waveform's its
    // partner on the same grid (Breeze's read as an arrow with a diamond beside it),
    // both the app's own, neither a theme icon.
    for (const auto &[button, name, icon] : {
             std::tuple{spectrogramButton, QStringLiteral("Spectrogram"), QStringLiteral("spectrogram-symbolic.svg")},
             std::tuple{waveformButton, QStringLiteral("Waveform"), QStringLiteral("waveform-symbolic.svg")}}) {
        QVERIFY(button->inherits("QQuickToolButton"));
        QCOMPARE(accessibleRole(button), QAccessible::RadioButton);
        QCOMPARE(accessibleText(button, QAccessible::Name), name);
        // The tool button's text is its tooltip.
        QCOMPARE(button->property("text").toString(), name);
        QQuickItem *glyph = button->findChild<QQuickItem *>(QStringLiteral("plotToolButtonIcon"));
        QVERIFY(glyph != nullptr);
        QVERIFY(glyph->property("isMask").toBool());
        QTRY_VERIFY2(glyph->property("valid").toBool(), qPrintable(name));
        QCOMPARE(evaluateOn(button, QStringLiteral("icon.name")).toString(), QString());
        const QString source = evaluateOn(button, QStringLiteral("icon.source")).toUrl().toString();
        QVERIFY2(source.endsWith(QStringLiteral("/assets/") + icon), qPrintable(source));
        QCOMPARE(glyph->property("source").toUrl().toString(), source);
    }
    {
        // The waveform glyph's 16 px source, on the spectrogram glyph's
        // grid: five vertical strokes of its weight, 2 px with round
        // caps, mirrored about the middle line, 4, 10, 14, 10 and 4 px tall
        // with their caps, an envelope that swells and falls back. They
        // stand 3 px apart with their edges and ends on whole pixels, so
        // each fills two whole columns at 1x, with a clear column between
        // neighbours (at 1.5 px each column was three quarters covered, and
        // the chosen glyph under 4.5:1); all inside the 16 px square.
        QFile file(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/waveform-symbolic.svg"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString svg = QString::fromUtf8(file.readAll());
        QVERIFY(svg.contains(QStringLiteral("viewBox=\"0 0 16 16\"")));
        QCOMPARE(svg.count(QStringLiteral("<path ")), 5);
        const QRegularExpression stroke(QStringLiteral(
            "stroke-width=\"([0-9.]+)\" stroke-linecap=\"round\" d=\"M([0-9.]+) ([0-9.]+)V([0-9.]+)\""));
        QList<double> xs;
        QList<double> heights;
        for (auto match = stroke.globalMatch(svg); match.hasNext();) {
            const QRegularExpressionMatch each = match.next();
            const double width = each.captured(1).toDouble();
            QCOMPARE(width, 2.0);
            const double top = each.captured(3).toDouble() - width / 2;
            const double bottom = each.captured(4).toDouble() + width / 2;
            // Mirrored about y 8, inside the square.
            QCOMPARE(top + bottom, 16.0);
            QVERIFY(top >= 0.0 && bottom <= 16.0);
            xs.push_back(each.captured(2).toDouble());
            heights.push_back(bottom - top);
        }
        QCOMPARE(heights, (QList<double>{4.0, 10.0, 14.0, 10.0, 4.0}));
        QCOMPARE(xs, (QList<double>{2.0, 5.0, 8.0, 11.0, 14.0}));
        // Edges and ends on whole pixels.
        for (int i = 0; i < xs.size(); ++i) {
            QCOMPARE(xs[i] - 1.0, std::floor(xs[i] - 1.0));
            QCOMPARE(heights[i] / 2, std::floor(heights[i] / 2));
        }
    }
    {
        // The spectrogram glyph's 16 px source, on the waveform glyph's
        // grid: filled outlines with every edge on a whole pixel and no
        // curve (drawn smooth it was the app's mark, which the idle row and
        // the stand-in cover use), inside the waveform's x 1 to 15 and
        // y 1 to 15. A band of five 2 px cells in the waveform's five
        // columns, a beat's pulses, under two 2 px traces that run the
        // whole width and step up only at a column's edge, never down: a
        // note's fundamental and its harmonic, rising twice as much, so
        // they fan apart, with a clear row or more between each part and
        // the next in every column (three ascending bars read as levels).
        QFile file(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/spectrogram-symbolic.svg"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString svg = QString::fromUtf8(file.readAll());
        QVERIFY(svg.contains(QStringLiteral("viewBox=\"0 0 16 16\"")));
        QVERIFY(!svg.contains(QStringLiteral("stroke=")));
        const QRegularExpression pathData(QStringLiteral("<path fill=\"#000\" d=\"([^\"]+)\"/>"));
        // Each path's pixels, by their centres, even-odd: only M, H, V and
        // Z with whole numbers.
        QList<std::array<std::array<bool, 16>, 16>> parts;
        for (auto match = pathData.globalMatch(svg); match.hasNext();) {
            const QString d = match.next().captured(1);
            QVERIFY2(QRegularExpression(QStringLiteral("^([MHVZ][0-9 ]*)+$")).match(d).hasMatch(), qPrintable(d));
            QList<QList<QPoint>> outlines;
            QPoint pen;
            const QRegularExpression command(QStringLiteral("([MHVZ])([0-9]*)(?: ([0-9]+))?"));
            for (auto each = command.globalMatch(d); each.hasNext();) {
                const QRegularExpressionMatch step = each.next();
                const QChar op = step.captured(1).front();
                if (op == QLatin1Char('M')) {
                    pen = QPoint(step.captured(2).toInt(), step.captured(3).toInt());
                    outlines.push_back({pen});
                } else if (op == QLatin1Char('H')) {
                    pen.setX(step.captured(2).toInt());
                    outlines.last().push_back(pen);
                } else if (op == QLatin1Char('V')) {
                    pen.setY(step.captured(2).toInt());
                    outlines.last().push_back(pen);
                }
            }
            std::array<std::array<bool, 16>, 16> inked{};
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    const QPointF centre(x + 0.5, y + 0.5);
                    bool inside = false;
                    for (const QList<QPoint> &outline : outlines) {
                        for (qsizetype i = 0; i < outline.size(); ++i) {
                            const QPointF a = outline[i];
                            const QPointF b = outline[(i + 1) % outline.size()];
                            if ((a.y() > centre.y()) != (b.y() > centre.y())
                                && centre.x() < a.x() + (centre.y() - a.y()) * (b.x() - a.x()) / (b.y() - a.y())) {
                                inside = !inside;
                            }
                        }
                    }
                    inked[y][x] = inside;
                }
            }
            parts.push_back(inked);
        }
        // The harmonic, the fundamental, the band.
        QCOMPARE(parts.size(), 3);
        const auto rows = [](const std::array<std::array<bool, 16>, 16> &inked, int x) {
            QList<int> found;
            for (int y = 0; y < 16; ++y) {
                if (inked[y][x])
                    found.push_back(y);
            }
            return found;
        };
        const QList<int> slices{1, 4, 7, 10, 13};
        for (int x = 0; x < 16; ++x) {
            const bool inColumn = slices.contains(x) || slices.contains(x - 1);
            // The band: one 2 px cell in each of the waveform's columns,
            // rows 13 and 14, the waveform's foot.
            QCOMPARE(rows(parts[2], x), inColumn ? (QList<int>{13, 14}) : QList<int>{});
        }
        QList<QList<int>> tops(2);
        for (int trace = 0; trace < 2; ++trace) {
            for (int x = 0; x < 16; ++x) {
                const QList<int> inked = rows(parts[trace], x);
                if (x < 1 || x > 14) {
                    QVERIFY(inked.isEmpty());
                    continue;
                }
                // 2 px, whole, and above the next part with a clear row.
                QCOMPARE(inked.size(), 2);
                QCOMPARE(inked[1], inked[0] + 1);
                QVERIFY(inked[0] >= 1);
                const QList<int> below = rows(parts[trace + 1], x);
                if (!below.isEmpty()) {
                    QVERIFY2(below.first() - inked[1] >= 2, qPrintable(QStringLiteral("trace %1, x %2").arg(trace).arg(x)));
                }
                tops[trace].push_back(inked[0]);
                // Steps only at a column's edge, and never down.
                if (x > 1) {
                    const int before = tops[trace][tops[trace].size() - 2];
                    QVERIFY(inked[0] <= before);
                    if (inked[0] != before) {
                        QVERIFY2(slices.contains(x), qPrintable(QStringLiteral("trace %1 steps at x %2").arg(trace).arg(x)));
                        // A step of a row, so the trace stays one line.
                        QCOMPARE(before - inked[0], 1);
                    }
                }
            }
        }
        // The harmonic rises twice as much as the fundamental: they fan.
        const int harmonicRise = tops[0].first() - tops[0].last();
        const int fundamentalRise = tops[1].first() - tops[1].last();
        QVERIFY(fundamentalRise > 0);
        QCOMPARE(harmonicRise, 2 * fundamentalRise);
    }
    // The checked one is the mode: a neutral plate, the plot's text colour
    // over the switch's plate, 3:1 on it (PlotToolButton's
    // segmentFillOpacity), with its glyph in the text colour whole; on a
    // light plate the dark text, 7:1, with its glyph in the plate's colour
    // (a mid-grey fill under a dark glyph read as a muddy box); the
    // other's glyph is at 72 % but while pressed (the accent's solid fill
    // read as keyboard focus; whole under the pointer, the other rivalled
    // the chosen one). Nothing of the switch is in the accent, or in the
    // plot's checked fill, without the keyboard's focus.
    const auto checkedFill = [](QQuickItem *button) {
        return button->findChild<QQuickItem *>(QStringLiteral("plotToolButtonChecked"));
    };
    const auto glyph = [](QQuickItem *button) {
        return button->findChild<QQuickItem *>(QStringLiteral("plotToolButtonIcon"));
    };
    QObject *tokens = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(tokens != nullptr);
    const QColor accent = tokens->property("accent").value<QColor>();
    const auto neutralPlate = [&](QQuickItem *chosen, QQuickItem *other, int mode) -> QString {
        QVariant set;
        QMetaObject::invokeMethod(tokens, "plotColors", Q_RETURN_ARG(QVariant, set), Q_ARG(QVariant, mode));
        QObject *plot = set.value<QObject *>();
        const QColor text = plot->property("plotText").value<QColor>();
        const QColor fill = checkedFill(chosen)->property("color").value<QColor>();
        const bool lightPlate = chosen->property("lightPlate").toBool();
        const QColor chosenInk = lightPlate ? chosen->property("plateColor").value<QColor>() : text;
        if (!chosen->property("segment").toBool() || !other->property("segment").toBool()) {
            return QStringLiteral("not segments");
        }
        if (!checkedFill(chosen)->isVisible() || checkedFill(other)->isVisible()) {
            return QStringLiteral("the chosen one's plate does not show, or the other's does");
        }
        if (fill.red() != text.red() || fill.green() != text.green() || fill.blue() != text.blue()
            || fill.alphaF() < (lightPlate ? 0.7f : 0.3f) || fill.alphaF() > (lightPlate ? 0.85f : 0.55f)
            || std::abs(fill.alphaF() - chosen->property("segmentFillOpacity").toFloat()) > 0.005f) {
            return QStringLiteral("the plate is %1").arg(fill.name(QColor::HexArgb));
        }
        if (fill == accent || fill == plot->property("checkedFill").value<QColor>()) {
            return QStringLiteral("the plate is the accent");
        }
        for (QQuickItem *button : {chosen, other}) {
            if (glyph(button)->property("color").value<QColor>() != (button == chosen ? chosenInk : text)) {
                return QStringLiteral("a glyph in %1").arg(glyph(button)->property("color").value<QColor>().name());
            }
        }
        if (glyph(chosen)->opacity() != 1.0) {
            return QStringLiteral("the chosen glyph at %1").arg(glyph(chosen)->opacity());
        }
        // Quiet with the keyboard's focus too, less so under the pointer:
        // whole only while pressed.
        const double quiet = other->property("hovered").toBool() ? 0.85 : 0.72;
        if (!other->property("down").toBool() && std::abs(glyph(other)->opacity() - quiet) > 0.005) {
            return QStringLiteral("the other glyph at %1").arg(glyph(other)->opacity());
        }
        // The focus ring, the plot's, shows only with the keyboard's focus.
        for (QQuickItem *button : {chosen, other}) {
            QQuickItem *ring = button->findChild<QQuickItem *>(QStringLiteral("plotToolButtonFocusRing"));
            if (ring->isVisible() != button->property("visualFocus").toBool()) {
                return QStringLiteral("a focus ring without the keyboard's focus");
            }
        }
        return {};
    };
    QVERIFY(spectrogramButton->property("checked").toBool());
    QVERIFY2(neutralPlate(spectrogramButton, waveformButton, 0).isEmpty(),
             qPrintable(neutralPlate(spectrogramButton, waveformButton, 0)));
    {
        // As drawn, on the spectrogram's plate: neutral, with nothing of
        // the accent's blue in it.
        QTest::mouseMove(window, QPoint(20, 20));
        QTest::qWait(50);
        const QImage image = window->grabWindow();
        const QColor drawn = drawnAt(image, window, spectrogramButton, QPointF(2.5, 2.5));
        QVERIFY2(std::abs(drawn.blue() - drawn.red()) <= 12 && std::abs(drawn.green() - drawn.red()) <= 12,
                 qPrintable(drawn.name()));
        const QColor plate = drawnAt(image, window, waveformButton, QPointF(waveformButton->width() - 2.5, 2.5));
        QVERIFY2(contrastRatio(drawn, plate) >= 1.4,
                 qPrintable(QStringLiteral("%1 on %2").arg(drawn.name(), plate.name())));
    }
    // The chosen one's 1 px edge, inside it, as drawn, in both modes' plot
    // colours: on a dark plate a quiet highlight over a fill that is 3:1 on
    // the plate and carries the choice (the fill alone was 1.4 to 1.7:1
    // once; an edge 3:1 on the fuller fill came to a near-white 10:1 wire
    // box on the plate); on a light plate the fill, 7:1 on the plate, is
    // its own edge.
    const auto edgeProblem = [&](QQuickItem *chosen, QQuickItem *other) -> QString {
        QQuickItem *edge = chosen->findChild<QQuickItem *>(QStringLiteral("plotToolButtonCheckedEdge"));
        QQuickItem *otherEdge = other->findChild<QQuickItem *>(QStringLiteral("plotToolButtonCheckedEdge"));
        if (edge == nullptr || !edge->isVisible() || otherEdge == nullptr || otherEdge->isVisible()) {
            return QStringLiteral("the edge does not show on the chosen one alone");
        }
        QTest::qWait(50);
        const QImage image = window->grabWindow();
        const QColor line = drawnAt(image, window, chosen, QPointF(chosen->width() / 2, 0.5));
        // The fill between the edge and the glyph.
        const QColor fill = drawnAt(image, window, chosen, QPointF(chosen->width() / 2, 2.5));
        const QColor plate = drawnAt(image, window, other, QPointF(other->width() - 2.5, 2.5));
        if (chosen->property("lightPlate").toBool()) {
            if (contrastRatio(fill, plate) < 6.9 || channelDistance(line, fill) > 3) {
                return QStringLiteral("edge %1 and fill %2 on plate %3").arg(line.name(), fill.name(), plate.name());
            }
            return {};
        }
        if (contrastRatio(fill, plate) < 3.0 || contrastRatio(line, plate) > 4.5
            || contrastRatio(line, fill) < 1.15 || contrastRatio(line, fill) > 1.4) {
            return QStringLiteral("edge %1 on plate %2 and fill %3").arg(line.name(), plate.name(), fill.name());
        }
        return {};
    };
    QVERIFY2(edgeProblem(spectrogramButton, waveformButton).isEmpty(),
             qPrintable(edgeProblem(spectrogramButton, waveformButton)));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      waveformButton->mapToScene(QPointF(12, 12)).toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QTRY_VERIFY(waveformButton->property("checked").toBool());
    QVERIFY(!spectrogramButton->property("checked").toBool());
    QVERIFY(checkedFill(waveformButton)->isVisible());
    QTest::mouseMove(window, QPoint(20, 20));
    QTRY_VERIFY2(neutralPlate(waveformButton, spectrogramButton, 1).isEmpty(),
                 qPrintable(neutralPlate(waveformButton, spectrogramButton, 1)));
    QTRY_VERIFY2(edgeProblem(waveformButton, spectrogramButton).isEmpty(),
                 qPrintable(edgeProblem(waveformButton, spectrogramButton)));
    // Space on the focused button sets the mode rather than playing.
    QVERIFY(tabTo(window, spectrogramButton));
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QVERIFY(spectrogramButton->property("checked").toBool());
}

// The mode switch's states stay apart: its buttons have as much plate
// between them as around them, so the keyboard's 2 px focus ring, with its
// 1 px gap in the plate's colour, stays inside the plate and clear of the
// chosen button's edge; the chosen glyph is whole, the other at 72 %, and under the
// pointer at 85 % on a 14 % wash, which shows on the plate and stays under
// the chosen one's plate. On both plots; in both Breeze themes,
// UiFoundationsTest's plotSegmentHoverStaysUnderTheChosenPlate and
// plotToolButtonDrawsCheckedAndFocusApart draw the hover and the ring.
void AccessibilityTest::checkModeSwitchStatesStayApart(QQuickWindow *window, int mode) {
    QObject *tokens = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(tokens != nullptr);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(viewer != nullptr);
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, mode)));
    QQuickItem *plate = visibleItemNamed(window, QStringLiteral("visualizationModeSwitch"));
    QQuickItem *chosen = visibleItemNamed(window, QStringLiteral("visualizationModeButton%1").arg(mode));
    QQuickItem *other = visibleItemNamed(window, QStringLiteral("visualizationModeButton%1").arg(1 - mode));
    QVERIFY(plate != nullptr && chosen != nullptr && other != nullptr);
    QTRY_VERIFY(chosen->property("checked").toBool());
    QVERIFY(!other->property("checked").toBool());
    const auto rectOf = [](QQuickItem *item) {
        return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    };
    const auto part = [](QQuickItem *button, const char *name) {
        return button->findChild<QQuickItem *>(QString::fromLatin1(name));
    };

    // An even plate: as much of it around the buttons as between them.
    QQuickItem *first = mode == 0 ? chosen : other;
    QQuickItem *second = mode == 0 ? other : chosen;
    const QRectF plateRect = rectOf(plate);
    const qreal inset = rectOf(first).left() - plateRect.left();
    QVERIFY(inset > 0);
    QCOMPARE(rectOf(first).top() - plateRect.top(), inset);
    QCOMPARE(plateRect.right() - rectOf(second).right(), inset);
    QCOMPARE(plateRect.bottom() - rectOf(second).bottom(), inset);
    QCOMPARE(rectOf(second).left() - rectOf(first).right(), inset);

    // The other one focused from the keyboard: its ring inside the plate,
    // 1 px or more clear of the chosen one's edge, with its gap.
    QVERIFY(tabTo(window, other));
    QTRY_VERIFY(other->property("visualFocus").toBool());
    QQuickItem *ring = part(other, "plotToolButtonFocusRing");
    QQuickItem *gap = part(other, "plotToolButtonFocusGap");
    QQuickItem *edge = part(chosen, "plotToolButtonCheckedEdge");
    QVERIFY(ring != nullptr && gap != nullptr && edge != nullptr);
    QTRY_VERIFY(ring->isVisible() && gap->isVisible() && edge->isVisible());
    const QRectF ringRect = rectOf(ring);
    // With 1 px of the plate outside it.
    QVERIFY2(plateRect.adjusted(1, 1, -1, -1).contains(ringRect),
             qPrintable(QStringLiteral("ring %1,%2 %3x%4 in plate %5,%6 %7x%8")
                            .arg(ringRect.left()).arg(ringRect.top()).arg(ringRect.width()).arg(ringRect.height())
                            .arg(plateRect.left()).arg(plateRect.top()).arg(plateRect.width())
                            .arg(plateRect.height())));
    const QRectF edgeRect = rectOf(edge);
    const double clearance = mode == 0 ? ringRect.left() - edgeRect.right() : edgeRect.left() - ringRect.right();
    QVERIFY2(clearance >= 1.0, qPrintable(QString::number(clearance)));
    // The 2 px ring round its 1 px gap, round the button.
    const qreal ringWidth = QQmlProperty(ring, QStringLiteral("border.width")).read().toReal();
    const qreal gapWidth = QQmlProperty(gap, QStringLiteral("border.width")).read().toReal();
    QCOMPARE(ringWidth, 2.0);
    QCOMPARE(gapWidth, 1.0);
    QCOMPARE(rectOf(other).left() - rectOf(gap).left(), gapWidth);
    QCOMPARE(rectOf(other).left() - ringRect.left(), gapWidth + ringWidth);

    // The glyphs: the chosen one whole, the other quiet.
    QQuickItem *chosenGlyph = part(chosen, "plotToolButtonIcon");
    QQuickItem *otherGlyph = part(other, "plotToolButtonIcon");
    QVERIFY(chosenGlyph != nullptr && otherGlyph != nullptr);
    QVariant set;
    QVERIFY(QMetaObject::invokeMethod(tokens, "plotColors", Q_RETURN_ARG(QVariant, set), Q_ARG(QVariant, mode)));
    QObject *plot = set.value<QObject *>();
    QVERIFY(plot != nullptr);
    const QColor text = plot->property("plotText").value<QColor>();
    QCOMPARE(chosenGlyph->property("color").value<QColor>(),
             chosen->property("lightPlate").toBool() ? chosen->property("plateColor").value<QColor>() : text);
    QCOMPARE(chosenGlyph->opacity(), 1.0);
    QVERIFY2(std::abs(otherGlyph->opacity() - 0.72) < 0.005, qPrintable(QString::number(otherGlyph->opacity())));

    // Under the pointer: a wash of some 16 to 24 %, its glyph at 85 %; as
    // drawn, it reads on the plate (at 10 %, 1.28:1, only the glyph told
    // it), under the chosen one's plate, which is 1.4:1 or more on it.
    QTest::mouseMove(window, rectOf(other).center().toPoint() + QPoint(0, 1));
    QTest::mouseMove(window, rectOf(other).center().toPoint());
    QTRY_VERIFY(other->property("hovered").toBool());
    QQuickItem *wash = part(other, "plotToolButtonHover");
    QQuickItem *fill = part(chosen, "plotToolButtonChecked");
    QVERIFY(wash != nullptr && fill != nullptr);
    QTRY_COMPARE(wash->opacity(), 1.0);
    QVERIFY2(std::abs(otherGlyph->opacity() - 0.85) < 0.005, qPrintable(QString::number(otherGlyph->opacity())));
    const QColor washColor = wash->property("color").value<QColor>();
    const QColor fillColor = fill->property("color").value<QColor>();
    QVERIFY2(washColor.alphaF() >= 0.155 && washColor.alphaF() <= 0.245, qPrintable(QString::number(washColor.alphaF())));
    QVERIFY(washColor.alphaF() < fillColor.alphaF());
    // The plate whole, as the pointer on it reveals it.
    const auto shownWhole = [&]() {
        double opacity = 1.0;
        for (QQuickItem *item = plate; item != nullptr; item = item->parentItem()) {
            opacity *= item->opacity();
        }
        return opacity == 1.0;
    };
    QTRY_VERIFY(shownWhole());
    QTest::qWait(50);
    const QImage image = window->grabWindow();
    const QColor drawnWash = drawnAt(image, window, other, QPointF(2.5, 2.5));
    const QColor drawnFill = drawnAt(image, window, chosen, QPointF(2.5, 2.5));
    // The plate beside the chosen one, clear of the other's focus ring.
    const QColor drawnPlate =
        drawnAt(image, window, plate, QPointF(mode == 0 ? 1.5 : plate->width() - 1.5, plate->height() / 2));
    const double washStep = contrastRatio(drawnWash, drawnPlate);
    const double fillStep = contrastRatio(drawnFill, drawnPlate);
    QVERIFY2(washStep >= 1.5 && washStep < fillStep,
             qPrintable(QStringLiteral("hovered %1:1, chosen %2:1 (%3, %4 on %5)").arg(washStep).arg(fillStep)
                            .arg(drawnWash.name(), drawnFill.name(), drawnPlate.name())));
    QVERIFY2(contrastRatio(drawnFill, drawnWash) >= 1.4,
             qPrintable(QStringLiteral("chosen %1 on hovered %2").arg(drawnFill.name(), drawnWash.name())));
}

void AccessibilityTest::checkVisualizationsHaveAccessibleRoles(QQuickWindow *window) {
    const QList<QPair<QString, QString>> items{
        {QStringLiteral("mainSpectrogramSurface"), QStringLiteral("Spectrogram")},
        {QStringLiteral("mainWaveformSurface"), QStringLiteral("Waveform")},
        {QStringLiteral("levelMeter"), QStringLiteral("Level meter")},
    };
    for (const auto &[objectName, name] : items) {
        auto *item = window->findChild<QQuickItem *>(objectName);
        QVERIFY2(item != nullptr, qPrintable(objectName));
        QCOMPARE(accessibleRole(item), QAccessible::Graphic);
        QCOMPARE(accessibleText(item, QAccessible::Name), name);
    }
}

namespace {
// The pixel of `image` at the scene point of `item`'s `point`.
QColor drawnAt(const QImage &image, const QQuickWindow *window, QQuickItem *item, QPointF point) {
    const double scale = double(image.width()) / window->width();
    const QPointF scene = item->mapToScene(point) * scale;
    return image.pixelColor(int(std::floor(scene.x())), int(std::floor(scene.y())));
}

// The pixel in `item`'s `rect` that contrasts most with `background`.
QColor mostContrasting(const QImage &image, const QQuickWindow *window, QQuickItem *item,
                       const QRectF &rect, const QColor &background) {
    QColor best = background;
    for (double y = rect.top(); y < rect.bottom(); y += 0.5) {
        for (double x = rect.left(); x < rect.right(); x += 0.5) {
            const QColor candidate = drawnAt(image, window, item, QPointF(x, y));
            if (contrastRatio(candidate, background) > contrastRatio(best, background)) best = candidate;
        }
    }
    return best;
}

QQuickItem *plotLayerOf(QQuickWindow *window) {
    return window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
}

// Two labelled lanes on the plot layer, as a stereo track's, with a track
// loaded: before any audio, the lanes at rest have no Mute and Solo.
bool showStereoLanes(QQuickItem *layer) {
    QObject *bridge = qvariant_cast<QObject *>(layer->property("uiBridge"));
    return bridge != nullptr && bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/track.flac"))
        && layer->setProperty("laneCount", 2)
        && layer->setProperty("laneLabels", QVariantList{QStringLiteral("L"), QStringLiteral("R")});
}
}

void AccessibilityTest::checkLaneButtonsAreTabStopsThatRevealThemselves(QQuickWindow *window) {
    QQuickItem *layer = plotLayerOf(window);
    QVERIFY(layer != nullptr);
    QVERIFY(showStereoLanes(layer));
    QList<QQuickItem *> plates;
    QTRY_COMPARE((plates = itemsNamed(layer, QStringLiteral("plotLaneButtons"))).size(), 2);
    const QList<QQuickItem *> mutes = itemsNamed(layer, QStringLiteral("plotLaneMuteButton"));
    const QList<QQuickItem *> solos = itemsNamed(layer, QStringLiteral("plotLaneSoloButton"));
    QCOMPARE(mutes.size(), 2);
    QCOMPARE(solos.size(), 2);
    // Hidden until wanted, but in the scene: Tab reaches them.
    for (QQuickItem *plate : plates) {
        QVERIFY(plate->isVisible());
        QCOMPARE(plate->opacity(), 0.0);
    }
    QCOMPARE(accessibleText(mutes[0], QAccessible::Name), QStringLiteral("Mute L"));
    QCOMPARE(accessibleText(solos[1], QAccessible::Name), QStringLiteral("Solo R"));
    QCOMPARE(accessibleRole(mutes[0]), QAccessible::CheckBox);
    // The tool button's text is its tooltip.
    QCOMPARE(solos[0]->property("text").toString(), QStringLiteral("Solo L"));
    // After the mode switch, each lane's Mute and Solo, in order.
    QQuickItem *waveformButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QVERIFY(tabTo(window, waveformButton));
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(mutes[0]->hasActiveFocus());
    // Focus in the lane shows its buttons, and only its.
    QTRY_COMPARE(plates[0]->opacity(), 1.0);
    QCOMPARE(plates[1]->opacity(), 0.0);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(solos[0]->hasActiveFocus());
    QCOMPARE(plates[0]->opacity(), 1.0);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(mutes[1]->hasActiveFocus());
    QTRY_COMPARE(plates[0]->opacity(), 0.0);
    QTRY_COMPARE(plates[1]->opacity(), 1.0);
    // The pointer on a lane shows its buttons too.
    QVERIFY(layer->setProperty("hoveredLane", 0));
    QTRY_COMPARE(plates[0]->opacity(), 1.0);
    QVERIFY(layer->setProperty("hoveredLane", -1));
    QTRY_COMPARE(plates[0]->opacity(), 0.0);
    // With the buttons set never to show, they are no Tab stops either.
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge->setProperty("channelButtonsVisibility", 0));
    QTRY_VERIFY(!plates[0]->isVisible());
    QVERIFY(!mutes[0]->isVisible());
    // Shown when wanted again, as by default.
    QVERIFY(bridge->setProperty("channelButtonsVisibility", 1));
}

// A lane's Mute and Solo tell assistive technology that they are toggles
// and whether each is on (only "focused, focusable" reached it). What they
// show is the lane's state: a click asks for the change and leaves the
// button as the lane is until the lane changes.
void AccessibilityTest::checkLaneButtonsAreCheckableToggles(QQuickWindow *window) {
    QQuickItem *layer = plotLayerOf(window);
    QVERIFY(layer != nullptr);
    QVERIFY(showStereoLanes(layer));
    QList<QQuickItem *> heads;
    QTRY_COMPARE((heads = itemsNamed(layer, QStringLiteral("plotLaneHead"))).size(), 2);
    QQuickItem *mute = itemsNamed(heads[0], QStringLiteral("plotLaneMuteButton")).value(0);
    QQuickItem *solo = itemsNamed(heads[1], QStringLiteral("plotLaneSoloButton")).value(0);
    QVERIFY(mute != nullptr && solo != nullptr);
    const auto problem = [&](QQuickItem *button, bool on) -> QString {
        QAccessibleInterface *iface = inAccessibleTree(window, button);
        if (iface == nullptr) {
            return QStringLiteral("%1 is not in the tree").arg(button->objectName());
        }
        if (iface->role() != QAccessible::CheckBox) {
            return QStringLiteral("%1 is a %2").arg(button->objectName()).arg(int(iface->role()));
        }
        const QAccessible::State state = iface->state();
        if (!state.checkable) {
            return QStringLiteral("%1 is not checkable").arg(button->objectName());
        }
        if (bool(state.checked) != on || button->property("checked").toBool() != on) {
            return QStringLiteral("%1 checked %2").arg(button->objectName()).arg(bool(state.checked));
        }
        return {};
    };
    QTRY_VERIFY2(problem(mute, false).isEmpty(), qPrintable(problem(mute, false)));
    QTRY_VERIFY2(problem(solo, false).isEmpty(), qPrintable(problem(solo, false)));

    // The lane muted, and the other soloed: each says so.
    QVERIFY(heads[0]->setProperty("muted", true));
    QVERIFY(heads[1]->setProperty("soloed", true));
    QTRY_VERIFY2(problem(mute, true).isEmpty(), qPrintable(problem(mute, true)));
    QTRY_VERIFY2(problem(solo, true).isEmpty(), qPrintable(problem(solo, true)));

    // A click asks the lane to change, and the button stays as the lane
    // is, which this test's bridge leaves alone; it still follows the lane.
    QVERIFY(layer->setProperty("hoveredLane", 0));
    QQuickItem *plate = itemsNamed(heads[0], QStringLiteral("plotLaneButtons")).value(0);
    QTRY_COMPARE(plate->opacity(), 1.0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      mute->mapToScene(QPointF(mute->width() / 2, mute->height() / 2)).toPoint());
    QTest::qWait(50);
    QVERIFY2(problem(mute, true).isEmpty(), qPrintable(problem(mute, true)));
    QVERIFY(heads[0]->setProperty("muted", false));
    QTRY_VERIFY2(problem(mute, false).isEmpty(), qPrintable(problem(mute, false)));

    // The head's plate: as even as the mode switch's, as much of it
    // around the buttons as between them and after the label's divider,
    // its top on one line with the switch's,
    // as far into the lane as the switch is into the plot, and a button's
    // focus ring inside it with 1 px of the plate outside it (at 2 px, the
    // ring ran past the plate's edge). The label is centred on it, so that L
    // and M stand on one centre line (with only the plates' tops on one
    // line, their letters were 6 px apart).
    const auto rectOf = [](QQuickItem *item) {
        return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    };
    QQuickItem *label = itemsNamed(heads[0], QStringLiteral("plotLaneLabel")).value(0);
    QQuickItem *firstSolo = itemsNamed(heads[0], QStringLiteral("plotLaneSoloButton")).value(0);
    QQuickItem *modeSwitch = itemsNamed(layer, QStringLiteral("visualizationModeSwitch")).value(0);
    QVERIFY(label != nullptr && firstSolo != nullptr && modeSwitch != nullptr);
    const QRectF plateRect = rectOf(plate);
    QQuickItem *divider = itemsNamed(plate, QStringLiteral("plotPlateDivider")).value(0);
    QVERIFY(divider != nullptr);
    const qreal inset = rectOf(mute).left() - rectOf(divider).right();
    QCOMPARE(inset, modeSwitch->property("padding").toReal());
    QCOMPARE(rectOf(mute).top() - plateRect.top(), inset);
    QCOMPARE(rectOf(firstSolo).left() - rectOf(mute).right(), inset);
    QCOMPARE(plateRect.right() - rectOf(firstSolo).right(), inset);
    QCOMPARE(plateRect.bottom() - rectOf(mute).bottom(), inset);
    QCOMPARE(plateRect.top(), rectOf(modeSwitch).top());
    const qreal plateInset = layer->property("plateInset").toReal();
    QVERIFY(plateInset > 0);
    QCOMPARE(plateRect.top() - rectOf(layer).top(), plateInset);
    QVERIFY2(std::abs(rectOf(label).center().y() - plateRect.center().y()) <= 0.5,
             qPrintable(QStringLiteral("label centre %1, plate centre %2")
                            .arg(rectOf(label).center().y()).arg(plateRect.center().y())));
    QVERIFY2(std::abs(rectOf(modeSwitch).center().y() - plateRect.center().y()) <= 0.5,
             qPrintable(QStringLiteral("switch centre %1, plate centre %2")
                            .arg(rectOf(modeSwitch).center().y()).arg(plateRect.center().y())));
    // The label as far in from the plate's left as from its top, so in the
    // lane's corner as far from the side as from the top (H26).
    QCOMPARE(plateRect.left() - rectOf(layer).left(), plateInset);
    QCOMPARE(rectOf(label).left() - plateRect.left(), rectOf(label).top() - plateRect.top());
    QCOMPARE(rectOf(label).left() - rectOf(layer).left(), rectOf(label).top() - rectOf(layer).top());
    mute->forceActiveFocus(Qt::TabFocusReason);
    QQuickItem *ring = mute->findChild<QQuickItem *>(QStringLiteral("plotToolButtonFocusRing"));
    QVERIFY(ring != nullptr);
    QTRY_VERIFY(ring->isVisible());
    QVERIFY2(plateRect.adjusted(1, 1, -1, -1).contains(rectOf(ring)),
             qPrintable(QStringLiteral("ring at %1,%2 in a plate at %3,%4")
                            .arg(rectOf(ring).left()).arg(rectOf(ring).top())
                            .arg(plateRect.left()).arg(plateRect.top())));
}

void AccessibilityTest::checkLevelMeterReservesItsColumnAndReadsOut(QQuickWindow *window) {
    QQuickItem *layer = plotLayerOf(window);
    QQuickItem *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    auto *meter = window->findChild<LevelMeterItem *>(QStringLiteral("levelMeter"));
    QVERIFY(layer != nullptr && surface != nullptr && bridge != nullptr && meter != nullptr);
    QVERIFY(showStereoLanes(layer));
    // Off: the plot is the pane's whole width.
    QVERIFY(!meter->isVisible());
    QCOMPARE(surface->property("plotWidth").toReal(), layer->width());
    QVERIFY(itemsNamed(layer, QStringLiteral("levelMeterLane")).isEmpty());
    // On: its column at the end of each lane, also with nothing playing.
    QVERIFY(bridge->setProperty("showLevelMeter", true));
    QTRY_VERIFY(meter->isVisible());
    const double column = LevelMeterItem::kColumnWidth;
    QCOMPARE(meter->width(), column);
    QCOMPARE(meter->laneCount(), 2);
    QVERIFY(!meter->active());
    QCOMPARE(surface->property("plotWidth").toReal(), layer->width() - column);
    // Play and stop leave the plot's width alone.
    for (const QString &state : {QStringLiteral("Playing"), QStringLiteral("Paused"), QStringLiteral("Stopped")}) {
        QVERIFY(bridge->setProperty("currentTrackPath", QStringLiteral("/music/a.flac")));
        QVERIFY(bridge->setProperty("playbackState", state));
        QCOMPARE(surface->property("plotWidth").toReal(), layer->width() - column);
    }
    // Each lane's column: its channel and level for assistive technology,
    // and the exact levels in its tooltip.
    const QList<QQuickItem *> lanes = itemsNamed(layer, QStringLiteral("levelMeterLane"));
    QCOMPARE(lanes.size(), 2);
    QCOMPARE(accessibleRole(lanes[0]), QAccessible::ProgressBar);
    QCOMPARE(accessibleText(lanes[0], QAccessible::Name), QStringLiteral("Level of L"));
    QCOMPARE(accessibleText(lanes[1], QAccessible::Name), QStringLiteral("Level of R"));
    QVERIFY(accessibleText(lanes[0], QAccessible::Description).contains(QStringLiteral("−∞ dB")));
    QVERIFY(bridge->setProperty("connected", true));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Playing")));
    QTRY_VERIFY(meter->active());
    // Held levels: the meter neither plays nor decays them while the
    // test reads them, and no clock of playback's clears them.
    meter->setPlaying(false);
    meter->setClock(nullptr);
    meter->m_channels.assign(2, {});
    meter->m_shown.assign(2, {});
    meter->m_shown[0].observe(-12.34, 0);
    meter->m_shown[1].observe(0.0, 0);
    meter->setReadoutWanted(true);
    QTRY_VERIFY(accessibleText(lanes[0], QAccessible::Description).contains(QStringLiteral("−12.3 dB")));
    QAccessibleInterface *value = QAccessible::queryAccessibleInterface(lanes[0]);
    QVERIFY(value != nullptr && value->valueInterface() != nullptr);
    QCOMPARE(value->valueInterface()->currentValue().toDouble(), -12.3);
    QCOMPARE(value->valueInterface()->minimumValue().toDouble(), -60.0);
    QCOMPARE(value->valueInterface()->maximumValue().toDouble(), 0.0);
    // Clipping is said, not only drawn.
    QVERIFY(accessibleText(lanes[1], QAccessible::Description).contains(QStringLiteral("clipping")));
    QCOMPARE(evaluateOn(lanes[1], QStringLiteral("ToolTip.text")).toString(),
             QStringLiteral("R: Level 0.0 dB, peak 0.0 dB, clipping"));
    // Paused, the column is an empty trough, as when stopped, and still
    // reserved; playing again, the meter comes back.
    const auto lampRow = [&]() {
        const QSharedPointer<QQuickItemGrabResult> grab = meter->grabToImage();
        QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
        const QImage image = ready.wait(5000) ? grab->image() : QImage();
        const double scale = image.isNull() ? 1.0 : double(image.width()) / meter->width();
        // The lamp's rows and the gap under it, 3 to 7 px into the lane.
        return QPair<QColor, QColor>(image.pixelColor(int(5 * scale), int(4.5 * scale)),
                                     image.pixelColor(int(5 * scale), int(6.5 * scale)));
    };
    // The trough as drawn on the plot's floor: the waveform's set, which
    // the bare light pane takes, has a translucent one.
    QObject *meterColors = layer->property("plotColors").value<QObject *>();
    const QColor trough = drawnOver(meterColors->property("meterTrough").value<QColor>(),
                                    meterColors->property("plotBg").value<QColor>());
    // Blending rounds each channel.
    const auto near = [](const QColor &a, const QColor &b) {
        return std::abs(a.red() - b.red()) <= 1 && std::abs(a.green() - b.green()) <= 1
            && std::abs(a.blue() - b.blue()) <= 1;
    };
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Paused")));
    QTRY_VERIFY(!meter->active());
    QCOMPARE(surface->property("plotWidth").toReal(), layer->width() - column);
    QVERIFY(meter->isVisible());
    auto [lamp, gap] = lampRow();
    QVERIFY2(near(lamp, trough), qPrintable(lamp.name() + QStringLiteral(" vs ") + trough.name()));
    QVERIFY2(near(gap, trough), qPrintable(gap.name() + QStringLiteral(" vs ") + trough.name()));
    QTRY_VERIFY(accessibleText(lanes[1], QAccessible::Description).contains(QStringLiteral("\u2212\u221e dB")));
    QVERIFY(!accessibleText(lanes[1], QAccessible::Description).contains(QStringLiteral("clipping")));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Playing")));
    QTRY_VERIFY(meter->active());
    std::tie(lamp, gap) = lampRow();
    QVERIFY(!near(lamp, trough));
    QVERIFY(!near(gap, trough));
    // Off again: the plot takes the column back.
    QVERIFY(bridge->setProperty("showLevelMeter", false));
    QTRY_COMPARE(surface->property("plotWidth").toReal(), layer->width());
}

void AccessibilityTest::plotLayerDrawsWithContrast() {
    MainWindowFixture main;
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *layer = plotLayerOf(window);
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *meter = window->findChild<LevelMeterItem *>(QStringLiteral("levelMeter"));
    QVERIFY(layer != nullptr && bridge != nullptr && viewer != nullptr && meter != nullptr);
    QVERIFY(showStereoLanes(layer));
    QVERIFY(bridge->setProperty("showLevelMeter", true));
    QVERIFY(bridge->setProperty("connected", true));
    QVERIFY(bridge->setProperty("currentTrackPath", QStringLiteral("/music/a.flac")));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Playing")));
    QTRY_VERIFY(meter->active());
    // The levels the test gives it decay, rather than play on, and no
    // clock of playback's clears them.
    meter->setPlaying(false);
    meter->setClock(nullptr);
    for (const int mode : {0, 1}) {
        QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, mode)));
        QTRY_COMPARE(viewer->property("visualizationMode").toInt(), mode);
        // The mode's plot gives the layer its lanes again: two, labelled.
        QVERIFY(showStereoLanes(layer));
        const QString what = mode == 0 ? QStringLiteral("spectrogram") : QStringLiteral("waveform");
        // A level of -12 dB in the first lane; it decays from there.
        meter->m_channels.assign(2, {});
        meter->m_channels[0].observe(-12, 0);
        meter->m_shown = meter->m_channels;
        meter->update();
        // Keyboard focus on the unchecked mode button.
        QQuickItem *unchecked = visibleItemNamed(
            window, QStringLiteral("visualizationModeButton%1").arg(mode == 0 ? 1 : 0));
        QQuickItem *checked = visibleItemNamed(window, QStringLiteral("visualizationModeButton%1").arg(mode));
        QVERIFY(tabTo(window, unchecked));
        QTRY_VERIFY(unchecked->property("visualFocus").toBool());
        QQuickItem *label = nullptr;
        for (QQuickItem *candidate : itemsNamed(layer, QStringLiteral("plotLaneLabel"))) {
            if (label == nullptr && candidate->isVisible()) label = candidate;
        }
        QVERIFY(label != nullptr);
        QQuickItem *labelText = label->findChild<QQuickItem *>(QStringLiteral("plotPlateLabel"));
        QQuickItem *switchPlate = visibleItemNamed(window, QStringLiteral("visualizationModeSwitch"));
        QVERIFY(label != nullptr && labelText != nullptr && switchPlate != nullptr);
        // The switch's plate beside the chosen mode, clear of the focused
        // one's ring, which reaches the plate's edge on its side.
        const QPointF plateSpot(mode == 0 ? 1.0 : switchPlate->width() - 1.0, switchPlate->height() / 2);
        // The switch whole before sampling it. Tabbing to the other segment
        // takes keyboard focus out of the switch and round the window, and
        // a track's loading ends the bare plot's reveal: the switch fades
        // out, and in again over shortDuration with the focus (PlotPlate's
        // revealLevel), its fill, its plate and both glyphs at the plate's
        // opacity. A wait for two like grabs in a row read it mid-fade, the
        // glyphs at some 45 %: QTRY's first two checks are only a qWait(0)
        // apart, in which no frame need render, and a grab advances no
        // animation.
        QTRY_COMPARE(switchPlate->opacity(), 1.0);
        // The level again, which decayed while the switch faded in.
        meter->m_channels.assign(2, {});
        meter->m_channels[0].observe(-12, 0);
        meter->m_shown = meter->m_channels;
        meter->update();
        const QImage image = window->grabWindow();
        // The label's text on its plate.
        const QColor plate = drawnAt(image, window, label, QPointF(1.5, 1.5));
        const QColor text = mostContrasting(image, window, labelText,
                                            QRectF(0, 0, labelText->width(), labelText->height()), plate);
        QVERIFY2(contrastRatio(text, plate) >= 4.5,
                 qPrintable(QStringLiteral("%1 label: %2").arg(what).arg(contrastRatio(text, plate))));
        // The chosen mode's plate on the switch's, a neutral step from it
        // (the plot's text colour at 20 %, not the accent's solid fill), its
        // glyph whole on it and the other's on the switch's plate; and the
        // focus ring.
        const QColor switchFill = drawnAt(image, window, switchPlate, plateSpot);
        // Between its 1 px edge, rounded at the corners, and its glyph.
        const QColor fill = drawnAt(image, window, checked, QPointF(checked->width() / 2, 2.5));
        QVERIFY2(contrastRatio(fill, switchFill) >= 1.4,
                 qPrintable(QStringLiteral("%1 checked: %2").arg(what).arg(contrastRatio(fill, switchFill))));
        QQuickItem *checkedGlyph = checked->findChild<QQuickItem *>(QStringLiteral("plotToolButtonIcon"));
        QQuickItem *uncheckedGlyph = unchecked->findChild<QQuickItem *>(QStringLiteral("plotToolButtonIcon"));
        QVERIFY(checkedGlyph != nullptr && uncheckedGlyph != nullptr);
        // A glyph's ink as drawn on `background`. A mask of the app's own,
        // from a file, as the spectrogram's, is coloured by the platform's
        // icon loader, which offscreen leaves it black, as the transport's
        // own glyphs are (TransportStatusTest): its colour at its opacity
        // over the background stands for it.
        const auto inkOf = [&](QQuickItem *glyph, const QColor &background) {
            if (glyph->property("source").metaType() != QMetaType::fromType<QUrl>()) {
                return mostContrasting(image, window, glyph, QRectF(0, 0, glyph->width(), glyph->height()),
                                       background);
            }
            const QColor color = glyph->property("color").value<QColor>();
            const double alpha = glyph->opacity();
            return QColor::fromRgbF(color.redF() * alpha + background.redF() * (1 - alpha),
                                    color.greenF() * alpha + background.greenF() * (1 - alpha),
                                    color.blueF() * alpha + background.blueF() * (1 - alpha));
        };
        const QColor checkedInk = inkOf(checkedGlyph, fill);
        QVERIFY2(contrastRatio(checkedInk, fill) >= 4.5,
                 qPrintable(QStringLiteral("%1 chosen glyph: %2 (%3 on %4)").arg(what)
                                .arg(contrastRatio(checkedInk, fill)).arg(checkedInk.name(), fill.name())));
        const QColor uncheckedInk = inkOf(uncheckedGlyph, switchFill);
        QVERIFY2(contrastRatio(uncheckedInk, switchFill) >= 3.0,
                 qPrintable(QStringLiteral("%1 other glyph: %2 (%3 on %4, %5 at %6)").arg(what)
                                .arg(contrastRatio(uncheckedInk, switchFill)).arg(uncheckedInk.name(), switchFill.name(),
                                     uncheckedGlyph->property("color").value<QColor>().name())
                                .arg(uncheckedGlyph->opacity())));
        const QColor ring = drawnAt(image, window, unchecked, QPointF(unchecked->width() / 2, -2.5));
        QVERIFY2(contrastRatio(ring, switchFill) >= 3.0,
                 qPrintable(QStringLiteral("%1 ring: %2").arg(what).arg(contrastRatio(ring, switchFill))));
        // The meter's fill against its trough, above the fill, as the meter
        // draws them: grabbed alone, as a window grab offscreen leaves the
        // right end of this wide window's pane undrawn.
        const QSharedPointer<QQuickItemGrabResult> meterGrab = meter->grabToImage();
        QVERIFY(meterGrab != nullptr);
        QSignalSpy ready(meterGrab.data(), &QQuickItemGrabResult::ready);
        QVERIFY(ready.wait(5000));
        const QImage meterImage = meterGrab->image();
        const double meterScale = double(meterImage.width()) / meter->width();
        const auto meterPixel = [&](double x, double y) {
            return meterImage.pixelColor(int(x * meterScale), int(y * meterScale));
        };
        const auto [top, bottom] = LevelMeterItem::laneRows(0, 2, meter->height());
        // The fill just under its top, where it meets the trough and shows
        // the level: on the spectrogram, a ramp of the colour map, whose
        // floor is darker. A few dB under the level, as the level decays
        // while the test runs; a rung of its ladder there, the trough
        // showing between them.
        const double meterBottom = bottom - LevelMeterItem::kInset;
        const double scaleHeight = bottom - top - 2 * LevelMeterItem::kInset - LevelMeterItem::kLampHeight - 1.0;
        // The trough on the track beside the body: above the level the
        // body holds the unlit ladder.
        const QColor trough = meterPixel(LevelMeterItem::kTrackLeft + 0.5, top + 9);
        QColor meterFill = trough;
        for (double offset = 0.5; offset < 3.0; offset += 1.0) {
            const QColor shown =
                meterPixel(5.5, std::floor(meterBottom - LevelMeterItem::meterPosition(-15, scaleHeight)) + offset);
            if (contrastRatio(shown, trough) > contrastRatio(meterFill, trough)) {
                meterFill = shown;
            }
        }
        // On the waveform, 3:1 on the trough. On the spectrogram the fill
        // stops at the map's magenta, as its orange and yellow were
        // louder than the plot: 3:1 on the plot's floor around the column
        // and 2:1 on the trough, the track, which is 1.3:1 on the floor to
        // show; the peak tick marks the level at full contrast (meterPeak,
        // near white).
        const double onTrough = contrastRatio(meterFill, trough);
        const QColor floor = layer->property("plotColors").value<QObject *>()->property("plotBg").value<QColor>();
        const double onFloor = contrastRatio(meterFill, floor);
        QVERIFY2(mode == 0 ? onTrough >= 2.0 && onFloor >= 3.0 : onTrough >= 3.0,
                 qPrintable(QStringLiteral("%1 meter: %2 on %3: %4, on the floor %5: %6")
                                .arg(what, meterFill.name(), trough.name()).arg(onTrough)
                                .arg(floor.name()).arg(onFloor)));
    }
}

void AccessibilityTest::checkPlotLayerShowsThePaneStates(QQuickWindow *window) {
    QQuickItem *layer = plotLayerOf(window);
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(layer != nullptr && bridge != nullptr && errors != nullptr);
    QQuickItem *analysing = layer->findChild<QQuickItem *>(QStringLiteral("plotAnalysingText"));
    QQuickItem *endsEarly = layer->findChild<QQuickItem *>(QStringLiteral("plotEndsEarly"));
    QVERIFY(analysing != nullptr && endsEarly != nullptr);
    // Before any audio: no message; the switch shows. The bare pane is the
    // spectrogram's floor, the plot's indigo, in the light theme the tests
    // run in too.
    QVERIFY(!analysing->isVisible());
    QVERIFY(!endsEarly->isVisible());
    QVERIFY(visibleItemNamed(window, QStringLiteral("visualizationModeSwitch")) != nullptr);
    QQuickItem *pane = window->findChild<QQuickItem *>(QStringLiteral("spectrogramPane"));
    QVERIFY(pane != nullptr);
    QCOMPARE(pane->property("color").value<QColor>(), QColor(QStringLiteral("#14062e")));
    // A track without its analysis yet: Analysing…, after a moment.
    QVERIFY(bridge->setProperty("currentTrackPath", QStringLiteral("/music/a.flac")));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Playing")));
    QVERIFY(layer->property("analysing").toBool());
    QVERIFY(!analysing->isVisible());
    QTRY_VERIFY(analysing->isVisible());
    QCOMPARE(analysing->property("text").toString(), QStringLiteral("Analysing…"));
    // Assistive technology reads it.
    QAccessibleInterface *analysingIface = inAccessibleTree(window, analysing);
    QVERIFY(analysingIface != nullptr);
    QCOMPARE(analysingIface->role(), QAccessible::StaticText);
    QCOMPARE(analysingIface->text(QAccessible::Name), QStringLiteral("Analysing…"));
    // The file ends early: its message, in the empty part after the cut,
    // as Recent Issues says it, with the way there.
    QVERIFY(bridge->setProperty("waveformCoverageSeconds", 242.6));
    QVERIFY(bridge->setProperty("waveformEndsEarly", true));
    QTRY_VERIFY(!analysing->isVisible());
    QVERIFY(layer->setProperty("markerX", 200.0));
    QTRY_VERIFY(endsEarly->isVisible());
    QCOMPARE(endsEarly->x(), 201.0);
    QQuickItem *text = layer->findChild<QQuickItem *>(QStringLiteral("plotEndsEarlyText"));
    QQuickItem *link = layer->findChild<QQuickItem *>(QStringLiteral("plotEndsEarlyLink"));
    QVERIFY(text != nullptr && link != nullptr);
    QCOMPARE(text->property("text").toString(), QStringLiteral("The file ends at 4:02."));
    QAccessibleInterface *textIface = inAccessibleTree(window, text);
    QVERIFY(textIface != nullptr);
    QCOMPARE(textIface->role(), QAccessible::StaticText);
    QCOMPARE(textIface->text(QAccessible::Name), QStringLiteral("The file ends at 4:02."));
    QCOMPARE(accessibleRole(link), QAccessible::Link);
    // The messages over the plot take no hover from the view under them, as
    // a Label of the app's style would, with its hover handler.
    const auto takesHover = [](QQuickItem *item) {
        const QList<QObject *> children = item->findChildren<QObject *>();
        return item->acceptHoverEvents()
            || std::any_of(children.cbegin(), children.cend(),
                           [](QObject *child) { return child->inherits("QQuickHoverHandler"); });
    };
    QVERIFY(!takesHover(analysing));
    QVERIFY(!takesHover(text));
    QSignalSpy issues(errors, SIGNAL(historyRequested()));
    link->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(issues.count(), 1);
    // No room after the cut: no message over the plot.
    QVERIFY(layer->setProperty("markerX", layer->width() - 40.0));
    QTRY_VERIFY(!endsEarly->isVisible());
    // The next track clears it.
    QVERIFY(bridge->setProperty("waveformEndsEarly", false));
    QVERIFY(layer->setProperty("markerX", 200.0));
    QTRY_VERIFY(!endsEarly->isVisible());
}

void AccessibilityTest::drawsOffscreenUnlessAPlatformIsChosen() {
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
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // Accessible interfaces exist while assistive technology is active.
    qputenv("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");
    // The basic render loop, whatever the environment asks for: the threaded
    // one installs an animation driver of its own for the GUI thread, and Qt
    // then takes no SteppedAnimationDriver.
    qputenv("QSG_RENDER_LOOP", "basic");

    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h. A child process that
    // transportBarHasNoWarningsUnderTheBasicStyle() starts asks for another.
    isolateFromDesktopSessionWithAppLook();
    const QString childStyle = qEnvironmentVariable(childControlsStyleVariable);
    if (!childStyle.isEmpty()) {
        QQuickStyle::setStyle(childStyle);
    }
    QApplication app(argc, argv);
    returnFocusFromClosedPopups();
    QAccessible::setActive(true);
    AccessibilityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_accessibility.moc"
