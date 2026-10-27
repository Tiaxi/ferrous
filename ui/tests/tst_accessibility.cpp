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
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "../src/BridgeClient.h"
#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"

namespace {

QMutex &capturedMessageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

QtMessageHandler &previousMessageHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void captureQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&capturedMessageMutex());
        capturedMessages().push_back(message);
    }
    if (previousMessageHandler()) {
        previousMessageHandler()(type, context, message);
    }
}

QString takeCapturedMessagesText() {
    QMutexLocker locker(&capturedMessageMutex());
    const QString text = capturedMessages().join('\n');
    capturedMessages().clear();
    return text;
}

// Drops the captured messages that are `message`.
void dropCapturedMessages(const QString &message) {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().removeAll(message);
}

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

// Loads Main.qml with its fallback bridge and makes it the active window.
QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel) {
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    window->requestActivate();
    return QTest::qWaitForWindowActive(window) ? window : nullptr;
}

QList<QQuickItem *> itemsNamed(QQuickItem *root, const QString &objectName) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (item->objectName() == objectName) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    visit(root);
    return found;
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

// Evaluates `expression` in `item`'s own QML scope, as for its attached
// properties.
QVariant evaluateOn(QQuickItem *item, const QString &expression) {
    QQmlExpression evaluated(qmlContext(item), item, expression);
    bool undefined = false;
    const QVariant value = evaluated.evaluate(&undefined);
    return evaluated.hasError() || undefined ? QVariant() : value;
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
    void everyPreferencesPageIsReachableByTab();
    void libraryAndQueueRowsHaveAccessibleNames();
    void slidersAndIconButtonsHaveNamesAndTooltips();
    void keyboardMovesOfTheSeekSliderSeek();
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
    void playbackMovesTheSeekBarByWholePixels();
    void seekBarStepsByTheWindowsDevicePixels();
    void transportBarHasNoWarnings();
    void transportBarHasNoWarningsUnderTheBasicStyle();
    void clicksLeaveSpaceToPlayOrPause();
    void modifiedKeysReachTheShortcutsFromFocusedButtons();
    void rowHeightsAndScrollingFollowTheFont();
    void queueHeaderAndRowsShareTheColumnWidths();
    void queueColumnsResizeByTheirHandles();
    void lightPaletteLeavesNoFixedColours();
    void tagEditorCaseButtonsAreBesideTheScrollBar();
    void modeSwitchIconFollowsTheMode();
    void visualizationsHaveAccessibleRoles();
};

void AccessibilityTest::initTestCase() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
}

void AccessibilityTest::init() {
    takeCapturedMessagesText();
}

void AccessibilityTest::cleanup() {
    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
}

void AccessibilityTest::cleanupTestCase() {
    qInstallMessageHandler(previousMessageHandler());
    previousMessageHandler() = nullptr;
}

void AccessibilityTest::everyPreferencesPageIsReachableByTab() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("preferencesDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QObject *pages = nullptr;
    QTRY_VERIFY((pages = visibleItemNamed(window, QStringLiteral("preferencesPages"))) != nullptr);
    QQuickItem *tabBar = visibleItemNamed(window, QStringLiteral("preferencesTabBar"));
    QVERIFY(tabBar != nullptr);
    const int pageCount = 5;
    QList<QQuickItem *> tabs;
    for (int i = 0; i < pageCount; ++i) {
        tabs.push_back(visibleItemNamed(window, QStringLiteral("preferencesTab_%1").arg(i)));
        QVERIFY(tabs.last() != nullptr);
        QCOMPARE(accessibleRole(tabs.last()), QAccessible::PageTab);
    }
    QCOMPARE(pages->property("currentIndex").toInt(), 0);

    // Tab moves through the tabs; Space opens the focused one.
    const auto tabTo = [&](QQuickItem *target) {
        for (int presses = 0; presses < 60 && !target->hasActiveFocus(); ++presses) {
            QTest::keyClick(window, Qt::Key_Tab);
        }
        return target->hasActiveFocus();
    };
    for (int i = 0; i < pageCount; ++i) {
        QVERIFY2(tabTo(tabs[i]), qPrintable(QStringLiteral("Tab did not reach tab %1").arg(i)));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(pages->property("currentIndex").toInt(), i);
    }

    // The arrow keys and Ctrl+Tab move between the pages.
    QVERIFY(tabs[pageCount - 1]->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 0);
    QVERIFY(tabs[0]->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), pageCount - 1);
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 1);
    QVERIFY(tabs[1]->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Tab, Qt::ControlModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 2);
    QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 0);
    QTest::keyClick(window, Qt::Key_Tab, Qt::ControlModifier);

    // The checkboxes on the pages take focus too, and Space toggles them.
    QTest::keyClick(window, Qt::Key_PageDown, Qt::ControlModifier);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 2);
    QQuickItem *levelMeterBox = visibleItemNamed(window, QStringLiteral("showLevelMeterCheckBox"));
    QVERIFY(levelMeterBox != nullptr);
    QVERIFY(levelMeterBox->property("checked").toBool());
    QVERIFY(tabTo(levelMeterBox));
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(!levelMeterBox->property("checked").toBool());
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(!bridge->property("showLevelMeter").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(visibleItemNamed(window, QStringLiteral("preferencesPages")) == nullptr);
}

void AccessibilityTest::libraryAndQueueRowsHaveAccessibleNames() {
    // Outlives the window that shows it.
    QueueRowsModel model;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
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

void AccessibilityTest::slidersAndIconButtonsHaveNamesAndTooltips() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("durationSeconds", 200.0);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("volume", 0.5);

    const QList<QPair<QString, QString>> buttons{
        {QStringLiteral("transportButton_previous"), QStringLiteral("Previous")},
        {QStringLiteral("transportButton_play"), QStringLiteral("Play")},
        {QStringLiteral("transportButton_pause"), QStringLiteral("Pause")},
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
             QStringLiteral("Stop (Ctrl+Shift+Space)"));

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
    QCOMPARE(accessibleText(seek, QAccessible::Description), QStringLiteral("00:00 of 03:20"));
    QAccessibleValueInterface *seekValue = QAccessible::queryAccessibleInterface(seek)->valueInterface();
    QVERIFY(seekValue != nullptr);
    QCOMPARE(seekValue->maximumValue().toDouble(), 200.0);
    QCOMPARE(accessibleRole(volume), QAccessible::Slider);
    QCOMPARE(accessibleText(volume, QAccessible::Name), QStringLiteral("Volume"));
    QTRY_COMPARE(accessibleText(volume, QAccessible::Description), QStringLiteral("50%"));
    QAccessibleValueInterface *volumeValue = QAccessible::queryAccessibleInterface(volume)->valueInterface();
    QVERIFY(volumeValue != nullptr);
    QCOMPARE(volumeValue->currentValue().toDouble(), 0.5);
}

void AccessibilityTest::keyboardMovesOfTheSeekSliderSeek() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
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

// Loads the transport harness with the focus on its start button.
QQuickWindow *loadTransportHarness(QQmlEngine &engine, QScopedPointer<QObject> &root, QString *errorText) {
    root.reset(createQml(engine, transportHarnessQml(), errorText));
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

} // namespace

void AccessibilityTest::arrowKeysStepTheVolumeFromItsValue() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(bridge != nullptr && volume != nullptr);
    QCOMPARE(volume->property("value").toDouble(), 0.5);
    QVERIFY(tabTo(window, volume));
    const auto volumesText = [&root]() {
        return describeValues(recordedValues(root.data(), "volumes"));
    };

    // Right steps up from the bridge's volume, and the bridge gets it.
    QTest::keyClick(window, Qt::Key_Right);
    QVERIFY2(onlyRecordedSince(root.data(), "volumes", 0, 0.6), qPrintable(volumesText()));
    bridge->setProperty("volume", 0.6);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 0.6));

    // Left steps back down.
    const qsizetype sent = recordedValues(root.data(), "volumes").size();
    QTest::keyClick(window, Qt::Key_Left);
    QVERIFY2(onlyRecordedSince(root.data(), "volumes", sent, 0.5), qPrintable(volumesText()));
    bridge->setProperty("volume", 0.5);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 0.5));
}

void AccessibilityTest::bridgeEchoesWaitForTheVolumeGesture() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(bridge != nullptr && volume != nullptr);
    QVERIFY(tabTo(window, volume));
    const auto volumesText = [&root]() {
        return describeValues(recordedValues(root.data(), "volumes"));
    };

    // A held arrow key: a stale echo neither moves the slider nor is sent.
    QTest::keyPress(window, Qt::Key_Right);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 0.6));
    bridge->setProperty("volume", 0.3);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 0.6));
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY2(onlyRecordedSince(root.data(), "volumes", 0, 0.6), qPrintable(volumesText()));
    bridge->setProperty("volume", 0.6);

    // A drag from a quarter to three quarters of the slider, likewise.
    const qsizetype sent = recordedValues(root.data(), "volumes").size();
    const qreal y = volume->height() / 2;
    const auto scenePoint = [volume, y](qreal fraction) {
        return volume->mapToScene(QPointF(volume->width() * fraction, y)).toPoint();
    };
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.25));
    for (int step = 1; step <= 10; ++step) {
        QTest::mouseMove(window, scenePoint(0.25 + 0.05 * step));
    }
    const double dragged = volume->property("value").toDouble();
    QVERIFY2(dragged > 0.6 && dragged < 0.9, qPrintable(QString::number(dragged)));
    bridge->setProperty("volume", 0.05);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), dragged));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.75));
    const QList<double> dragVolumes = recordedValues(root.data(), "volumes").mid(sent);
    QVERIFY2(!dragVolumes.isEmpty() && fuzzyEqual(dragVolumes.constLast(), dragged),
             qPrintable(volumesText()));
    QVERIFY2(std::none_of(dragVolumes.begin(), dragVolumes.end(),
                          [](double value) { return value < 0.15; }),
             qPrintable(volumesText()));
}

void AccessibilityTest::arrowKeysSeekFromThePlaybackPosition() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    QCOMPARE(seek->property("value").toDouble(), 50.0);
    QVERIFY(tabTo(window, seek));
    const auto seeks = [&root]() { return recordedValues(root.data(), "seeks"); };

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
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
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
    const double dragged = seek->property("value").toDouble();
    QVERIFY2(dragged > 130.0 && dragged < 170.0, qPrintable(QString::number(dragged)));
    root->setProperty("positionSeconds", 51.0);
    QCOMPARE(seek->property("value").toDouble(), dragged);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.75));
    QCOMPARE(recordedValues(root.data(), "seeks"), (QList<double>{dragged}));
}

void AccessibilityTest::heldArrowKeysAccumulateVolumeSteps() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QVERIFY(bridge != nullptr && volume != nullptr);
    bridge->setProperty("volume", 0.2);
    root->setProperty("echoLags", true);
    QVERIFY(tabTo(window, volume));

    // Each step is sent once, from the one before, though the echo lags.
    holdKey(window, Qt::Key_Right, 6);
    const QList<double> volumes = recordedValues(root.data(), "volumes");
    QVERIFY2(sameValues(volumes, {0.3, 0.4, 0.5, 0.6, 0.7, 0.8}), qPrintable(describeValues(volumes)));
    bridge->setProperty("volume", 0.8);
    QVERIFY(fuzzyEqual(volume->property("value").toDouble(), 0.8));

    holdKey(window, Qt::Key_Left, 3);
    const QList<double> after = recordedValues(root.data(), "volumes").mid(6);
    QVERIFY2(sameValues(after, {0.7, 0.6, 0.5}), qPrintable(describeValues(after)));
}

void AccessibilityTest::heldArrowKeysSeekOnceWhenReleased() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    root->setProperty("echoLags", true);
    QVERIFY(tabTo(window, seek));
    const auto seeks = [&root]() { return recordedValues(root.data(), "seeks"); };

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
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QQuickItem *volume = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(volume != nullptr && seek != nullptr);
    root->setProperty("echoLags", true);

    // Right's release leaves Left held, so the lagging echo does not reset
    // the next step.
    QVERIFY(tabTo(window, volume));
    overlapArrowKeys(window);
    const QList<double> volumes = recordedValues(root.data(), "volumes");
    QVERIFY2(sameValues(volumes, {0.4, 0.3, 0.2, 0.3, 0.2}), qPrintable(describeValues(volumes)));
    QVERIFY(!volume->property("adjusting").toBool());

    // The seek slider seeks once, when the last key is released.
    QVERIFY(tabTo(window, seek));
    overlapArrowKeys(window);
    QCOMPARE(recordedValues(root.data(), "seeks"), (QList<double>{35.0}));
    QVERIFY(!seek->property("adjusting").toBool());
}

// Loads the transport harness with a lagging echo and the focus on the seek
// slider.
QQuickWindow *loadSeekHarness(QQmlEngine &engine, QScopedPointer<QObject> &root, QString *errorText) {
    QQuickWindow *window = loadTransportHarness(engine, root, errorText);
    if (window == nullptr) {
        return nullptr;
    }
    root->setProperty("echoLags", true);
    return tabTo(window, visibleItemNamed(window, QStringLiteral("seekSlider"))) ? window : nullptr;
}

void AccessibilityTest::seekDragOutlastsAKeyRelease() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadSeekHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
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
    const double dragged = seek->property("value").toDouble();
    QVERIFY2(dragged > 80.0 && dragged < 120.0, qPrintable(QString::number(dragged)));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.5));
    const QList<double> seeks = recordedValues(root.data(), "seeks");
    QVERIFY2(sameValues(seeks, {dragged}), qPrintable(describeValues(seeks)));
    QVERIFY(!seek->property("adjusting").toBool());
}

void AccessibilityTest::seekDragOutlastsTheFocus() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadSeekHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    auto *start = qvariant_cast<QQuickItem *>(root->property("startButton"));
    const qreal y = seek->height() / 2;
    const auto scenePoint = [seek, y](qreal fraction) {
        return seek->mapToScene(QPointF(seek->width() * fraction, y)).toPoint();
    };
    const auto seeksText = [&root]() {
        return describeValues(recordedValues(root.data(), "seeks"));
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
    const double dragged = seek->property("value").toDouble();
    QVERIFY2(dragged > 130.0 && dragged < 170.0, qPrintable(QString::number(dragged)));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scenePoint(0.75));
    QVERIFY2(sameValues(recordedValues(root.data(), "seeks"), {dragged}), qPrintable(seeksText()));
    QVERIFY(!seek->property("adjusting").toBool());

    // The key's release, which reaches the button, seeks nothing more.
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY2(sameValues(recordedValues(root.data(), "seeks"), {dragged}), qPrintable(seeksText()));
}

void AccessibilityTest::seekClickSeeksOnce() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(seek != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      seek->mapToScene(QPointF(seek->width() * 0.75, seek->height() / 2)).toPoint());
    const QList<double> seeks = recordedValues(root.data(), "seeks");
    QVERIFY2(seeks.size() == 1 && seeks.constFirst() > 130.0 && seeks.constFirst() < 170.0,
             qPrintable(describeValues(seeks)));
}

void AccessibilityTest::playbackMovesTheSeekBarByWholePixels() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QQuickItem *handle = visibleItemNamed(window, QStringLiteral("seekHandle"));
    QQuickItem *progress = visibleItemNamed(window, QStringLiteral("seekProgress"));
    QVERIFY(seek != nullptr && handle != nullptr && progress != nullptr);
    int frames = 0;
    connect(window, &QQuickWindow::frameSwapped, window, [&frames]() { ++frames; });
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

    // Three seconds of playback in 60 position updates a second. At 1x, the
    // bar moves about 3 device pixels a second, and the time label once.
    const double startSeconds = 50.0;
    const double durationSeconds = 200.0;
    const int steps = 180;
    struct Drawn {
        qreal handleX;
        qreal progressWidth;
        QString positionText;
        bool operator==(const Drawn &) const = default;
    };
    const auto drawn = [&]() {
        return Drawn{handle->x(), progress->width(), root->property("positionText").toString()};
    };
    Drawn previous = drawn();
    int changes = 0;
    frames = 0;
    for (int step = 1; step <= steps; ++step) {
        root->setProperty("positionSeconds", startSeconds + step / 60.0);
        const Drawn now = drawn();
        changes += now == previous ? 0 : 1;
        previous = now;
        QTest::qWait(16);
    }
    QVERIFY(settle());
    const QString counts = QStringLiteral("%1 frames, %2 drawn changes").arg(frames).arg(changes);
    qInfo("seek bar playback: %s in %d position updates", qPrintable(counts), steps);
    QVERIFY2(changes >= 3 && changes < steps / 4 && frames <= changes, qPrintable(counts));

    // The value stays exact, and the bar is within a device pixel of it.
    const double endSeconds = startSeconds + steps / 60.0;
    QVERIFY(fuzzyEqual(seek->property("value").toDouble(), endSeconds));
    const qreal devicePixel = 1.0 / window->effectiveDevicePixelRatio();
    const qreal exactWidth = progress->parentItem()->width() * endSeconds / durationSeconds;
    QVERIFY2(std::abs(progress->width() - exactWidth) <= devicePixel,
             qPrintable(QStringLiteral("%1 vs %2").arg(progress->width()).arg(exactWidth)));

    // A held key draws its value as it is.
    QVERIFY(tabTo(window, seek));
    QTest::keyPress(window, Qt::Key_Right);
    QVERIFY(seek->property("adjusting").toBool());
    QVERIFY(fuzzyEqual(seek->property("value").toDouble(), endSeconds + 5.0));
    QVERIFY(fuzzyEqual(progress->width(),
                       progress->parentItem()->width() * (endSeconds + 5.0) / durationSeconds));
    QTest::keyRelease(window, Qt::Key_Right);
    QVERIFY(sameValues(recordedValues(root.data(), "seeks"), {endSeconds + 5.0}));

    // A wheel step seeks at once, as the desktop style's wheel does, and the
    // bar draws the seek's position.
    root->setProperty("positionSeconds", endSeconds + 5.0);
    seek->setProperty("wheelEnabled", true);
    QVERIFY(sendWheel(window, seek->mapToScene(QPointF(seek->width() / 2, seek->height() / 2)),
                      QPoint(0, 120)));
    const QList<double> seeks = recordedValues(root.data(), "seeks");
    QVERIFY2(sameValues(seeks, {endSeconds + 5.0, endSeconds + 10.0}), qPrintable(describeValues(seeks)));
    root->setProperty("positionSeconds", endSeconds + 10.0);
    QVERIFY(std::abs(progress->width()
                     - progress->parentItem()->width() * (endSeconds + 10.0) / durationSeconds)
            <= devicePixel);
}

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
    QQuickItem *progress = itemsNamed(harness, QStringLiteral("seekProgress")).value(0);
    QVERIFY(progress != nullptr && progress->parentItem()->width() > 500);

    // Each step the bar takes in five seconds of playback is one of the
    // window's device pixels.
    const auto checkSteps = [&](double fromSeconds) {
        root->setProperty("positionSeconds", fromSeconds);
        QList<qreal> steps;
        qreal width = progress->width();
        for (int step = 1; step <= 250; ++step) {
            root->setProperty("positionSeconds", fromSeconds + step / 50.0);
            if (progress->width() != width) {
                steps.push_back((progress->width() - width) * window.effectiveDevicePixelRatio());
                width = progress->width();
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

void AccessibilityTest::transportBarHasNoWarnings() {
    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QString errorText;
    QQuickWindow *window = loadTransportHarness(engine, root, &errorText);
    QVERIFY2(window != nullptr, qPrintable(errorText));
    qInfo("Qt Quick Controls style: %s", qPrintable(QQuickStyle::name()));
    // Lay the bar out again at other sizes.
    window->resize(640, 160);
    QTRY_COMPARE(window->width(), 640);
    window->resize(1400, 240);
    QTRY_COMPARE(window->width(), 1400);
    QQuickItem *handle = visibleItemNamed(window, QStringLiteral("seekHandle"));
    QQuickItem *seek = visibleItemNamed(window, QStringLiteral("seekSlider"));
    QVERIFY(handle != nullptr && seek != nullptr);
    qInfo("seek slider height %g, handle height %g", seek->height(), handle->height());
    // cleanup() fails on any warning, such as a binding loop.
}

void AccessibilityTest::transportBarHasNoWarningsUnderTheBasicStyle() {
    // The style is fixed once a control loads, so a child process runs the
    // check above under the Basic style.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QUICK_CONTROLS_STYLE"), QStringLiteral("Basic"));
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

void AccessibilityTest::clicksLeaveSpaceToPlayOrPause() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
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

void AccessibilityTest::modifiedKeysReachTheShortcutsFromFocusedButtons() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
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
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
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
    QObject *metadataCard = nullptr;
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->property("tableRowHeight").isValid()) {
            tagEditor = object;
        }
        if (object->property("labelColumnWidth").isValid()) {
            metadataCard = object;
        }
    }
    QVERIFY(tagEditor != nullptr && metadataCard != nullptr);
    const qreal defaultLabelWidth = metadataCard->property("labelColumnWidth").toReal();

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
    // The now-playing labels' column widens with them.
    QTRY_VERIFY(metadataCard->property("labelColumnWidth").toReal() > defaultLabelWidth * 1.6);
    dropMinimumSizeWarnings();
}

void AccessibilityTest::queueHeaderAndRowsShareTheColumnWidths() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
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

    // A larger font widens the columns, which stay aligned.
    QFont larger = window->property("font").value<QFont>();
    larger.setPointSizeF((larger.pointSizeF() > 0 ? larger.pointSizeF() : 10.0) * 1.5);
    QVERIFY(window->setProperty("font", larger));
    QList<qreal> scaled;
    QTRY_VERIFY2((scaled = alignedWidths()).size() == columns.size()
                     && scaled[3] > defaults[3] * 1.3,
                 qPrintable(problem));
    QVERIFY(scaled[1] > defaults[1] * 1.3);
    QVERIFY(scaled[4] > defaults[4] * 1.3);
    QVERIFY(scaled[5] > defaults[5] * 1.3);
    dropMinimumSizeWarnings();
}

void AccessibilityTest::queueColumnsResizeByTheirHandles() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
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

// A light theme's window with the transport bar and the tag editor.
static QByteArray lightHarnessQml() {
    return QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
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
            dirty: false; selected: true; errorText: ""; extraValues: ""
        }
        ListElement {
            fileName: "b.flac"; directory: "/music"; formatKind: "FLAC"; title: "B"
            artist: "X"; album: "Y"; genre: ""; year: ""; trackNo: "2"; discNo: ""
            dirty: false; selected: false; errorText: ""; extraValues: ""
        }
        ListElement {
            fileName: "c.flac"; directory: "/music"; formatKind: "FLAC"; title: "C"
            artist: "X"; album: "Y"; genre: ""; year: ""; trackNo: "3"; discNo: ""
            dirty: false; selected: false; errorText: ""; extraValues: ""
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
        function extraValuesNote(field) { return "" }
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

    // The seek bar.
    QQuickItem *background = visibleItemNamed(window, QStringLiteral("seekBackground"));
    QVERIFY(background != nullptr);
    QCOMPARE(background->property("color").value<QColor>(), QColor(QStringLiteral("#fffbf2")));
    QCOMPARE(background->property("color").value<QColor>(), paletteColor("uiSurfaceRaisedColor"));
    QCOMPARE(evaluateOn(background, QStringLiteral("border.color")).value<QColor>(),
             paletteColor("uiBorderColor"));
    QQuickItem *progress = visibleItemNamed(window, QStringLiteral("seekProgress"));
    QVERIFY(progress != nullptr);
    QVERIFY(sameRgb(progress->property("color").value<QColor>(), paletteColor("uiAccentColor")));
    for (const QString &name : {QStringLiteral("seekPlayhead"), QStringLiteral("seekHandle")}) {
        QQuickItem *item = visibleItemNamed(window, name);
        QVERIFY2(item != nullptr, qPrintable(name));
        QCOMPARE(item->property("color").value<QColor>(), paletteColor("uiAccentColor"));
    }
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
    QCOMPARE(waveforms.first()->property("barColor").value<QColor>(), paletteColor("uiWaveformColor"));
    QCOMPARE(waveforms.first()->property("bgColor").value<QColor>(), paletteColor("uiSurfaceRaisedColor"));
    QQuickItem *mute = visibleItemNamed(window, QStringLiteral("muteButton"));
    QVERIFY(mute != nullptr);
    QCOMPARE(evaluateOn(mute, QStringLiteral("icon.color")).value<QColor>(), paletteColor("uiTextColor"));

    // The tag editor's rows and status.
    QObject *tagEditor = qvariant_cast<QObject *>(root->property("tagEditor"));
    QVERIFY(QMetaObject::invokeMethod(tagEditor, "open"));
    QList<QQuickItem *> rows;
    QTRY_COMPARE((rows = visibleRows(window->contentItem(), QStringLiteral("tagEditorRow"))).size(), 3);
    QCOMPARE(rows[0]->property("color").value<QColor>(), paletteColor("uiSelectionColor"));
    QCOMPARE(rows[1]->property("color").value<QColor>(), paletteColor("uiSurfaceAltColor"));
    QCOMPARE(rows[2]->property("color").value<QColor>(), paletteColor("uiSurfaceRaisedColor"));
    QQuickItem *chip = visibleItemNamed(window, QStringLiteral("tagEditorStatusChip"));
    QVERIFY(chip != nullptr);
    QVERIFY(sameRgb(chip->property("color").value<QColor>(),
                    chip->property("toneColor").value<QColor>()));
}

void AccessibilityTest::tagEditorCaseButtonsAreBesideTheScrollBar() {
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createQml(engine, lightHarnessQml(), &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    window->resize(1200, 500);
    QObject *tagEditor = qvariant_cast<QObject *>(root->property("tagEditor"));
    QVERIFY(QMetaObject::invokeMethod(tagEditor, "open"));
    QQuickItem *caseButton = nullptr;
    QTRY_VERIFY((caseButton = visibleItemNamed(window, QStringLiteral("tagEditorCaseButton_title"))) != nullptr);
    // The fields' scroll view is the button's nearest scrolling ancestor.
    QQuickItem *scrollView = caseButton->parentItem();
    while (scrollView != nullptr && !scrollView->inherits("QQuickScrollView")) {
        scrollView = scrollView->parentItem();
    }
    QVERIFY(scrollView != nullptr);
    auto *bar = qvariant_cast<QQuickItem *>(evaluateOn(scrollView, QStringLiteral("ScrollBar.vertical")));
    QVERIFY(bar != nullptr);
    QTRY_VERIFY(bar->isVisible());
    // Breeze puts the scroll bar beside the content, which the Basic
    // style overlays.
    QVERIFY(scrollView->setProperty("rightPadding", bar->width()));
    QTRY_VERIFY(tagEditor->property("opened").toBool());
    // The field's row spans the width beside the scroll bar.
    QTRY_COMPARE(caseButton->parentItem()->width(),
                 scrollView->property("availableWidth").toReal());
    const qreal buttonRight = caseButton->mapToScene(QPointF(caseButton->width(), 0)).x();
    const qreal barLeft = bar->mapToScene(QPointF()).x();
    QVERIFY2(buttonRight <= barLeft + 0.5,
             qPrintable(QStringLiteral("button ends at %1, the scroll bar starts at %2")
                            .arg(buttonRight).arg(barLeft)));
}

void AccessibilityTest::modeSwitchIconFollowsTheMode() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QQuickItem *waveformButton = visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QVERIFY(viewer != nullptr && waveformButton != nullptr);
    QObject *stroke = waveformButton->findChild<QObject *>(
        QStringLiteral("visualizationModeWaveformStroke"));
    QVERIFY2(stroke != nullptr, "The waveform icon is not drawn from a bound colour");
    const QColor unselected = stroke->property("strokeColor").value<QColor>();
    QCOMPARE(unselected, waveformButton->property("iconColor").value<QColor>());

    QJSValue setMode = qvariant_cast<QJSValue>(
        waveformButton->parentItem()->property("setMode"));
    QVERIFY(setMode.isCallable());
    setMode.call(QJSValueList{QJSValue(1)});
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QTRY_VERIFY(stroke->property("strokeColor").value<QColor>() != unselected);
    QCOMPARE(stroke->property("strokeColor").value<QColor>(),
             waveformButton->property("iconColor").value<QColor>());
    setMode.call(QJSValueList{QJSValue(0)});
    QTRY_COMPARE(stroke->property("strokeColor").value<QColor>(), unselected);
}

void AccessibilityTest::visualizationsHaveAccessibleRoles() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
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

int main(int argc, char **argv) {
    QTemporaryDir testStorage;
    if (!testStorage.isValid()) return 1;
    qputenv("XDG_DATA_HOME", QFile::encodeName(testStorage.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(testStorage.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(testStorage.filePath("cache")));
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // Accessible interfaces exist while assistive technology is active.
    qputenv("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");

    QApplication app(argc, argv);
    QAccessible::setActive(true);
    AccessibilityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_accessibility.moc"
