// SPDX-License-Identifier: GPL-3.0-or-later

// Keyboard reach, accessible names, font scaling and theme colours.

#include <QAccessible>
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QJSValue>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <cmath>
#include <functional>

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
    void rowHeightsAndScrollingFollowTheFont();
    void queueHeaderAndRowsShareTheColumnWidths();
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
    // The arrow keys seek by the keyboard's seek step.
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE(bridge->property("lastSeekSeconds").toDouble(), 5.0);
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE(bridge->property("lastSeekSeconds").toDouble(), 10.0);
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

    QFont larger = window->property("font").value<QFont>();
    larger.setPointSizeF((larger.pointSizeF() > 0 ? larger.pointSizeF() : 10.0) * 2);
    QVERIFY(window->setProperty("font", larger));
    QTRY_VERIFY(queue->property("rowHeight").toReal() > defaultRowHeight * 1.6);
    check(queue->property("rowHeight").toReal());
    // The now-playing labels' column widens with them.
    QTRY_VERIFY(metadataCard->property("labelColumnWidth").toReal() > defaultLabelWidth * 1.6);
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
