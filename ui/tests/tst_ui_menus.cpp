// SPDX-License-Identifier: GPL-3.0-or-later

// The menus of #373: the menu bar's menus with their words, icons, shortcuts
// and order; the Repeat and Visualization headings, which are text the
// pointer and the keyboard pass over; the radios, which follow the backend
// and the plot's mode switch; the items that turn off when they cannot act;
// the window's shortcuts, each taken once; and the context menus of the
// queue and the library.

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QKeySequence>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlListReference>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <functional>
#include <tuple>

#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"
#include "../src/WindowPresence.h"
#include "TestPlatform.h"

namespace {

QMutex &messageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

QtMessageHandler &previousHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&messageMutex());
        capturedMessages().push_back(message);
    }
    if (previousHandler()) {
        previousHandler()(type, context, message);
    }
}

QString takeMessages() {
    QMutexLocker locker(&messageMutex());
    capturedMessages().removeAll(QStringLiteral("This plugin does not support propagateSizeHints()"));
    const QString text = capturedMessages().join(QLatin1Char('\n'));
    capturedMessages().clear();
    return text;
}

// Main.qml with its fallback bridge, shown and active.
QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel) {
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    window->resize(1600, 1000);
    window->requestActivate();
    if (!QTest::qWaitForWindowActive(window)
        || !QTest::qWaitFor([&]() { return window->width() == 1600 && window->height() == 1000; })) {
        return nullptr;
    }
    return window;
}

QObject *bridgeOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiBridge"));
}

QObject *child(QQuickWindow *window, const char *name) {
    return window->findChild<QObject *>(QString::fromLatin1(name));
}

// The items named `name` under `root`, depth first.
QList<QQuickItem *> itemsNamed(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (item->objectName() == name) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    if (root != nullptr) {
        visit(root);
    }
    return found;
}

QQuickItem *named(QQuickWindow *window, const char *name) {
    const QList<QQuickItem *> found = itemsNamed(window->contentItem(), QString::fromLatin1(name));
    return found.isEmpty() ? nullptr : found.first();
}

// The first text item under `root` that shows `text`.
QQuickItem *textItem(QQuickItem *root, const QString &text) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->inherits("QQuickText") && root->property("text").toString() == text) {
        return root;
    }
    for (QQuickItem *child : root->childItems()) {
        if (QQuickItem *found = textItem(child, text)) {
            return found;
        }
    }
    return nullptr;
}

QRectF sceneRect(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

QObject *menuBarOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("menuBar"));
}

QList<QObject *> menusOf(QQuickWindow *window) {
    QList<QObject *> menus;
    QQmlListReference list(menuBarOf(window), "menus");
    for (qsizetype i = 0; i < list.count(); ++i) {
        menus.push_back(list.at(i));
    }
    return menus;
}

QList<QQuickItem *> menuItems(QObject *menu) {
    QList<QQuickItem *> items;
    const int count = menu->property("count").toInt();
    for (int i = 0; i < count; ++i) {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, i));
        items.push_back(item);
    }
    return items;
}

QString withoutMnemonic(QString text) {
    text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    text.remove(QLatin1Char('&'));
    text.replace(QStringLiteral("\x01"), QStringLiteral("&"));
    return text;
}

// A shortcut as text, from a string or a standard key.
QString shortcutText(const QVariant &shortcut) {
    if (!shortcut.isValid() || shortcut.isNull()) {
        return {};
    }
    if (shortcut.typeId() == QMetaType::Int) {
        return QKeySequence(static_cast<QKeySequence::StandardKey>(shortcut.toInt())).toString();
    }
    return QKeySequence(shortcut.toString()).toString();
}

QString standardKey(QKeySequence::StandardKey key) {
    return QKeySequence(key).toString();
}

QString iconOf(QQuickItem *item) {
    const QString name = QQmlProperty::read(item, QStringLiteral("icon.name")).toString();
    return name.isEmpty() ? QQmlProperty::read(item, QStringLiteral("icon.source")).toUrl().toString() : name;
}

// What a menu shows, one row each: "sep", "heading|Repeat", or
// "kind|text|shortcut|icon", where kind is item, check, radio or hint (an
// item that shows a key it does not take). Hidden items are left out.
QStringList rowsOf(QObject *menu) {
    QStringList rows;
    // A closed menu's items are all hidden; an open one's, only those it
    // leaves out.
    const bool menuShown = menu->property("visible").toBool();
    for (QQuickItem *item : menuItems(menu)) {
        if (item == nullptr) {
            rows << QStringLiteral("null");
            continue;
        }
        if (menuShown && !item->isVisible()) {
            continue;
        }
        if (item->inherits("QQuickMenuSeparator")) {
            rows << QStringLiteral("sep");
            continue;
        }
        if (item->property("isHeading").toBool()) {
            rows << QStringLiteral("heading|") + item->property("text").toString();
            continue;
        }
        QString kind = QStringLiteral("item");
        QString shortcut;
        auto *action = qvariant_cast<QObject *>(item->property("action"));
        if (action != nullptr) {
            shortcut = shortcutText(action->property("shortcut"));
        }
        if (item->property("keyHint").isValid()) {
            kind = QStringLiteral("hint");
            shortcut = item->property("keyHint").toString();
        }
        if (item->property("checkable").toBool()) {
            auto *indicator = qvariant_cast<QQuickItem *>(item->property("indicator"));
            kind = indicator != nullptr && indicator->property("elementType").toString() == QStringLiteral("radiobutton")
                ? QStringLiteral("radio")
                : QStringLiteral("check");
        }
        rows << QStringLiteral("%1|%2|%3|%4")
                    .arg(kind, withoutMnemonic(item->property("text").toString()), shortcut, iconOf(item));
    }
    return rows;
}

QObject *menuNamed(QQuickWindow *window, const char *objectName) {
    for (QObject *menu : menusOf(window)) {
        if (menu->objectName() == QLatin1String(objectName)) {
            return menu;
        }
    }
    return nullptr;
}

QQuickItem *menuItemWithText(QObject *menu, const QString &text) {
    for (QQuickItem *item : menuItems(menu)) {
        if (item != nullptr && withoutMnemonic(item->property("text").toString()) == text) {
            return item;
        }
    }
    return nullptr;
}

int indexOfItem(QObject *menu, QQuickItem *item) {
    return static_cast<int>(menuItems(menu).indexOf(item));
}

// Every action of the window, in the order declared.
QList<QObject *> actionsOf(QQuickWindow *window) {
    QList<QObject *> actions;
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->inherits("QQuickAction")) {
            actions.push_back(object);
        }
    }
    return actions;
}

// Title Case as KDE writes it: every word capitalized but the short words
// in the middle, and names as they are spelled.
bool isTitleCase(const QString &text) {
    static const QStringList small{QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("and"),
                                   QStringLiteral("as"), QStringLiteral("at"), QStringLiteral("by"),
                                   QStringLiteral("for"), QStringLiteral("from"), QStringLiteral("in"),
                                   QStringLiteral("of"), QStringLiteral("on"), QStringLiteral("or"),
                                   QStringLiteral("the"), QStringLiteral("to"), QStringLiteral("with")};
    QString plain = text;
    plain.remove(QChar(0x2026));
    const QStringList words = plain.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < words.size(); ++i) {
        const QString &word = words.at(i);
        if (i > 0 && small.contains(word)) {
            continue;
        }
        if (!word.at(0).isUpper() && word != QStringLiteral("iTunes")) {
            return false;
        }
    }
    return true;
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

const QString kTrackPath = QStringLiteral("/music/Artist A/Album A/02.flac");
const QString kArtistKey = QStringLiteral("artist|Artist A");
const QString kAlbumKey = QStringLiteral("album|Artist A|Album A");

// An artist with an album of one track, in the bridge's tree format.
QByteArray artistAlbumTrackTree() {
    struct Row {
        quint8 rowType;
        quint16 depth;
        quint32 trackNumber;
        quint32 childCount;
        qint32 year;
        QString title;
        QString key;
        QString path;
        QString trackPath;
    };
    const QList<Row> rows{
        {1, 0, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, QStringLiteral("/music/Artist A"), {}},
        {2, 1, 0, 1, 2005, QStringLiteral("Album A"), kAlbumKey, QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackPath, kTrackPath, kTrackPath},
    };
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const Row &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, -1);
        appendLe<quint32>(out, row.trackNumber);
        appendLe<quint32>(out, row.childCount);
        appendLe<qint32>(out, row.year);
        appendUtf8(out, row.title);
        appendUtf8(out, row.key);
        appendUtf8(out, QStringLiteral("Artist A"));
        appendUtf8(out, row.path);
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

QVariantList sampleQueueRows(int count) {
    QVariantList rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(QVariantMap{
            {QStringLiteral("title"), QStringLiteral("Track %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist %1").arg(i)},
            {QStringLiteral("album"), QStringLiteral("Album")},
            {QStringLiteral("lengthText"), QStringLiteral("1:00")},
            {QStringLiteral("path"), QStringLiteral("/music/%1.flac").arg(i)},
        });
    }
    return rows;
}

// The row of the view `viewName` at `index`, as the view shows it.
QQuickItem *viewRow(QQuickWindow *window, const char *viewName, int index) {
    QQuickItem *view = named(window, viewName);
    QQuickItem *row = nullptr;
    if (view != nullptr) {
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
    }
    return row;
}

QObject *visibleMenu(QQuickWindow *window, const QString &objectName) {
    for (QObject *menu : window->findChildren<QObject *>(objectName)) {
        if (menu->property("visible").toBool()) {
            return menu;
        }
    }
    return nullptr;
}

void rightClick(QQuickWindow *window, QQuickItem *item) {
    const QPoint at = sceneRect(item).center().toPoint();
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, at);
}

} // namespace

class UiMenusTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void menuBarHasTheSixKdeMenusInOrder();
    void menusListTheirItemsInOrder();
    void wordingIsTitleCaseWithOneCharacterEllipsis();
    void playPauseItemNamesWhatItDoes();
    void headingsAreTextThatIsNotPicked();
    void headingsAreSkippedByThePointerAndTheKeyboard();
    void repeatRadiosFollowTheBackendAndSetIt();
    void visualizationRadiosFollowThePlotSwitch();
    void scaleItemIsOffInWaveformMode();
    void editMenuNamesItsUndoAndClearsTheQueue();
    void findOpensSearchOnCtrlF();
    void shortcutsAreTakenOnceEach();
    void shortcutsTriggerTheirActions();
    void queueContextMenuShowsKeysItDoesNotTake();
    void libraryContextMenuNamesWhatItActsOn();
    void menusHaveNoTransitionsOfTheirOwn();
    void settingsAndHelpOpenTheirPages();
};

void UiMenusTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(captureMessage);
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
}

void UiMenusTest::init() {
    takeMessages();
}

void UiMenusTest::cleanup() {
    const QString messages = takeMessages();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiMenusTest::cleanupTestCase() {
    qInstallMessageHandler(previousHandler());
    previousHandler() = nullptr;
}

void UiMenusTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiMenusTest::menuBarHasTheSixKdeMenusInOrder() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    QStringList titles;
    for (QObject *menu : menusOf(window)) {
        titles << menu->property("title").toString();
    }
    QCOMPARE(titles, (QStringList{QStringLiteral("&File"), QStringLiteral("&Edit"), QStringLiteral("&View"),
                                  QStringLiteral("&Playback"), QStringLiteral("&Settings"),
                                  QStringLiteral("&Help")}));
}

void UiMenusTest::menusListTheirItemsInOrder() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    QCOMPARE(rowsOf(menuNamed(window, "fileMenu")),
             (QStringList{
                 QStringLiteral("item|Open Files…|%1|document-open").arg(standardKey(QKeySequence::Open)),
                 QStringLiteral("item|Add Files to Queue…||media-playlist-append"),
                 QStringLiteral("item|Add Folder to Queue…||folder-add"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Add Library Folder…||folder-music"),
                 QStringLiteral("item|Play Whole Library||media-playback-start"),
                 QStringLiteral("item|Queue Whole Library||media-playlist-append"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Quit|%1|application-exit").arg(standardKey(QKeySequence::Quit)),
             }));
    // No Redo: the queue's undo has none yet.
    QCOMPARE(rowsOf(menuNamed(window, "editMenu")),
             (QStringList{
                 QStringLiteral("item|Undo|%1|edit-undo").arg(standardKey(QKeySequence::Undo)),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Find…|%1|edit-find").arg(standardKey(QKeySequence::Find)),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Clear Queue||edit-clear-list"),
             }));
    QCOMPARE(rowsOf(menuNamed(window, "viewMenu")),
             (QStringList{
                 QStringLiteral("item|Open Visualization Viewer|F11|view-media-visualization"),
                 QStringLiteral("item|Open Album Art Viewer|Shift+F11|view-preview"),
                 QStringLiteral("sep"),
                 QStringLiteral("heading|Visualization"),
                 QStringLiteral("radio|Spectrogram|Ctrl+1|"),
                 QStringLiteral("radio|Waveform|Ctrl+2|"),
                 QStringLiteral("sep"),
                 QStringLiteral("check|Show Level Meter||"),
                 QStringLiteral("check|Show Frequency and Time Scale||"),
                 QStringLiteral("sep"),
                 QStringLiteral("check|Follow Current Track in Queue||"),
                 QStringLiteral("item|Reset Visualization View||zoom-original"),
             }));
    QCOMPARE(rowsOf(menuNamed(window, "playbackMenu")),
             (QStringList{
                 QStringLiteral("item|Play|Space|media-playback-start"),
                 QStringLiteral("item|Stop|Ctrl+Shift+Space|media-playback-stop"),
                 QStringLiteral("item|Previous|Ctrl+Left|media-skip-backward"),
                 QStringLiteral("item|Next|Ctrl+Right|media-skip-forward"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Seek Backward|Shift+Left|media-seek-backward"),
                 QStringLiteral("item|Seek Forward|Shift+Right|media-seek-forward"),
                 QStringLiteral("sep"),
                 QStringLiteral("check|Shuffle||"),
                 QStringLiteral("heading|Repeat"),
                 QStringLiteral("radio|Off||"),
                 QStringLiteral("radio|One Track||"),
                 QStringLiteral("radio|Whole Queue||"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Volume Up|Alt+Up|audio-volume-high"),
                 QStringLiteral("item|Volume Down|Alt+Down|audio-volume-low"),
                 QStringLiteral("check|Mute|Ctrl+Shift+M|"),
             }));
    QCOMPARE(rowsOf(menuNamed(window, "settingsMenu")),
             (QStringList{
                 QStringLiteral("item|Configure Ferrous…|%1|configure").arg(standardKey(QKeySequence::Preferences)),
             }));
    QCOMPARE(rowsOf(menuNamed(window, "helpMenu")),
             (QStringList{
                 QStringLiteral("item|Report Bug…||tools-report-bug"),
                 QStringLiteral("item|Diagnostics…||view-list-details"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|About Ferrous||qrc:/icons/hicolor/16x16/apps/ferrous.svg"),
                 QStringLiteral("item|About KDE||kde"),
             }));
    // The app icon's drawing is built in.
    QVERIFY(QFile::exists(QStringLiteral(":/icons/hicolor/16x16/apps/ferrous.svg")));
}

void UiMenusTest::wordingIsTitleCaseWithOneCharacterEllipsis() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    libraryModel.setExpandedKeys({kArtistKey, kAlbumKey});
    libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree());

    QList<QObject *> menus = menusOf(window);
    menus << window->findChildren<QObject *>(QStringLiteral("playlistContextMenu"));
    menus << window->findChildren<QObject *>(QStringLiteral("libraryContextMenu"));
    QCOMPARE(menus.size(), 8);
    int clearQueueItems = 0;
    for (QObject *menu : std::as_const(menus)) {
        for (QQuickItem *item : menuItems(menu)) {
            if (item == nullptr || item->inherits("QQuickMenuSeparator")) {
                continue;
            }
            const QString text = withoutMnemonic(item->property("text").toString());
            QVERIFY2(!text.contains(QStringLiteral("...")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("Playlist"), Qt::CaseInsensitive), qPrintable(text));
            QVERIFY2(isTitleCase(text), qPrintable(text));
            if (text == QStringLiteral("Clear Queue")) {
                ++clearQueueItems;
            }
        }
    }
    // In one menu only: Edit.
    QCOMPARE(clearQueueItems, 1);
    for (QObject *action : actionsOf(window)) {
        if (action->parent() != window) {
            continue;
        }
        const QString text = withoutMnemonic(action->property("text").toString());
        QVERIFY2(!text.contains(QStringLiteral("...")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("Playlist"), Qt::CaseInsensitive), qPrintable(text));
        QVERIFY2(isTitleCase(text), qPrintable(text));
    }
}

void UiMenusTest::playPauseItemNamesWhatItDoes() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *item = menuItems(menuNamed(window, "playbackMenu")).value(0);
    QVERIFY(item != nullptr);

    QCOMPARE(withoutMnemonic(item->property("text").toString()), QStringLiteral("Play"));
    QCOMPARE(iconOf(item), QStringLiteral("media-playback-start"));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(withoutMnemonic(item->property("text").toString()), QStringLiteral("Pause"));
    QCOMPARE(iconOf(item), QStringLiteral("media-playback-pause"));
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(withoutMnemonic(item->property("text").toString()), QStringLiteral("Play"));
}

void UiMenusTest::headingsAreTextThatIsNotPicked() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *playback = menuNamed(window, "playbackMenu");
    QObject *view = menuNamed(window, "viewMenu");
    QMetaObject::invokeMethod(playback, "open");
    QTRY_VERIFY(playback->property("opened").toBool());

    for (const auto &[menu, name, firstChoice] :
         {std::tuple{playback, "repeatHeading", QStringLiteral("Off")},
          std::tuple{view, "visualizationHeading", QStringLiteral("Spectrogram")}}) {
        auto *heading = menu->findChild<QQuickItem *>(QString::fromLatin1(name));
        QVERIFY2(heading != nullptr, name);
        // Not a disabled item: an enabled one that cannot be picked.
        QVERIFY(heading->isEnabled());
        QCOMPARE(heading->opacity(), 1.0);
        QVERIFY(!heading->property("hoverEnabled").toBool());
        QVERIFY(!heading->activeFocusOnTab());
        QCOMPARE(heading->property("focusPolicy").toInt(), int(Qt::NoFocus));
        QVERIFY(qvariant_cast<QObject *>(heading->property("action")) == nullptr);
        QCOMPARE(heading->property("font").value<QFont>().weight(), QFont::DemiBold);

        // In the text colour of the items under it, at full opacity.
        QQuickItem *choice = menuItemWithText(menu, firstChoice);
        QVERIFY(choice != nullptr);
        QQuickItem *headingText = textItem(heading, heading->property("text").toString());
        QQuickItem *choiceText = textItem(choice, withoutMnemonic(choice->property("text").toString()));
        if (menu == playback) {
            QVERIFY(headingText != nullptr && choiceText != nullptr);
            QCOMPARE(headingText->property("color").value<QColor>(), choiceText->property("color").value<QColor>());
            QCOMPARE(headingText->opacity(), 1.0);
            // In the text column, where the items' text starts.
            QCOMPARE(sceneRect(headingText).left(), sceneRect(choiceText).left());
            // Low in its row, near the items it heads.
            QVERIFY(sceneRect(headingText).center().y() > sceneRect(heading).center().y());
        }

        // A heading to assistive technology, not an item to pick.
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(heading);
        QVERIFY(iface != nullptr);
        QCOMPARE(iface->role(), QAccessible::Heading);
        QCOMPARE(iface->text(QAccessible::Name), heading->property("text").toString());
        // The choices say what they choose.
        QAccessibleInterface *choiceInterface = QAccessible::queryAccessibleInterface(choice);
        QVERIFY(choiceInterface != nullptr);
        QCOMPARE(choiceInterface->text(QAccessible::Description), heading->property("text").toString());
    }
    QMetaObject::invokeMethod(playback, "close");
}

void UiMenusTest::headingsAreSkippedByThePointerAndTheKeyboard() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *menu = menuNamed(window, "playbackMenu");
    auto *heading = menu->findChild<QQuickItem *>(QStringLiteral("repeatHeading"));
    QQuickItem *shuffle = menuItemWithText(menu, QStringLiteral("Shuffle"));
    QQuickItem *off = menuItemWithText(menu, QStringLiteral("Off"));
    QVERIFY(heading != nullptr && shuffle != nullptr && off != nullptr);

    QMetaObject::invokeMethod(menu, "open");
    QTRY_VERIFY(menu->property("opened").toBool());
    QQuickWindow *menuWindow = heading->window();
    QVERIFY(menuWindow != nullptr);

    // The pointer: no highlight, and a click neither picks nor closes.
    const QPoint headingAt = heading->mapToScene(QPointF(heading->width() / 2, heading->height() / 2)).toPoint();
    QTest::mouseMove(menuWindow, headingAt);
    QTest::qWait(50);
    QVERIFY(!heading->property("highlighted").toBool());
    QSignalSpy triggered(heading, SIGNAL(triggered()));
    QTest::mouseClick(menuWindow, Qt::LeftButton, Qt::NoModifier, headingAt);
    QTest::qWait(50);
    QCOMPARE(triggered.count(), 0);
    QVERIFY(menu->property("visible").toBool());
    QCOMPARE(bridge->property("repeatMode").toInt(), 0);

    // The keyboard goes from Shuffle to Off and back.
    menu->setProperty("currentIndex", indexOfItem(menu, shuffle));
    QTRY_COMPARE(menu->property("currentIndex").toInt(), indexOfItem(menu, shuffle));
    QTest::keyClick(menuWindow, Qt::Key_Down);
    QTRY_COMPARE(menu->property("currentIndex").toInt(), indexOfItem(menu, off));
    QVERIFY(off->hasActiveFocus());
    QTest::keyClick(menuWindow, Qt::Key_Up);
    QTRY_COMPARE(menu->property("currentIndex").toInt(), indexOfItem(menu, shuffle));
    QVERIFY(shuffle->hasActiveFocus());
    // And over separators and disabled items, as in KDE's other menus:
    // nothing plays, so there is nothing to seek in.
    QQuickItem *wholeQueue = menuItemWithText(menu, QStringLiteral("Whole Queue"));
    QQuickItem *volumeUp = menuItemWithText(menu, QStringLiteral("Volume Up"));
    QQuickItem *next = menuItemWithText(menu, QStringLiteral("Next"));
    QVERIFY(wholeQueue != nullptr && volumeUp != nullptr && next != nullptr);
    menu->setProperty("currentIndex", indexOfItem(menu, wholeQueue));
    QTRY_VERIFY(wholeQueue->hasActiveFocus());
    QTest::keyClick(menuWindow, Qt::Key_Down);
    QTRY_COMPARE(menu->property("currentIndex").toInt(), indexOfItem(menu, volumeUp));
    QVERIFY(volumeUp->hasActiveFocus());
    QVERIFY(!menuItemWithText(menu, QStringLiteral("Seek Forward"))->isEnabled());
    menu->setProperty("currentIndex", indexOfItem(menu, next));
    QTRY_VERIFY(next->hasActiveFocus());
    QTest::keyClick(menuWindow, Qt::Key_Down);
    QTRY_COMPARE(menu->property("currentIndex").toInt(), indexOfItem(menu, shuffle));
    QTest::keyClick(menuWindow, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
}

void UiMenusTest::repeatRadiosFollowTheBackendAndSetIt() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    const QList<QObject *> radios{child(window, "repeatOffAction"), child(window, "repeatOneAction"),
                                  child(window, "repeatAllAction")};
    for (QObject *radio : radios) {
        QVERIFY(radio != nullptr);
    }
    const auto checkedIndex = [&]() {
        int index = -1;
        for (qsizetype i = 0; i < radios.size(); ++i) {
            if (radios.at(i)->property("checked").toBool()) {
                if (index >= 0) {
                    return -2;
                }
                index = static_cast<int>(i);
            }
        }
        return index;
    };
    QCOMPARE(checkedIndex(), 0);

    // The backend's mode, however it changed.
    bridge->setProperty("repeatMode", 2);
    QTRY_COMPARE(checkedIndex(), 2);
    bridge->setProperty("repeatMode", 1);
    QTRY_COMPARE(checkedIndex(), 1);

    // A choice sets the backend, and stays the only one.
    QMetaObject::invokeMethod(radios.at(2), "trigger");
    QTRY_COMPARE(bridge->property("repeatMode").toInt(), 2);
    QCOMPARE(checkedIndex(), 2);
    QMetaObject::invokeMethod(radios.at(0), "trigger");
    QTRY_COMPARE(bridge->property("repeatMode").toInt(), 0);
    QCOMPARE(checkedIndex(), 0);
    // Picking the chosen one keeps it.
    QMetaObject::invokeMethod(radios.at(0), "trigger");
    QCOMPARE(bridge->property("repeatMode").toInt(), 0);
    QCOMPARE(checkedIndex(), 0);

    // Shuffle is a checkbox of its own.
    QObject *shuffle = child(window, "shuffleAction");
    QMetaObject::invokeMethod(shuffle, "trigger");
    QTRY_VERIFY(bridge->property("shuffleEnabled").toBool());
    QVERIFY(shuffle->property("checked").toBool());
    QCOMPARE(checkedIndex(), 0);
}

void UiMenusTest::visualizationRadiosFollowThePlotSwitch() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *viewer = child(window, "viewerController");
    QObject *spectrogram = child(window, "spectrogramModeAction");
    QObject *waveform = child(window, "waveformModeAction");
    QQuickItem *spectrogramButton = named(window, "visualizationModeButton0");
    QQuickItem *waveformButton = named(window, "visualizationModeButton1");
    QVERIFY(viewer != nullptr && spectrogram != nullptr && waveform != nullptr);
    QVERIFY(spectrogramButton != nullptr && waveformButton != nullptr);
    QVERIFY(spectrogram->property("checked").toBool());
    QVERIFY(!waveform->property("checked").toBool());

    // The menu moves the plot's switch.
    QMetaObject::invokeMethod(waveform, "trigger");
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QVERIFY(waveformButton->property("checked").toBool());
    QVERIFY(!spectrogramButton->property("checked").toBool());
    QVERIFY(waveform->property("checked").toBool());
    QVERIFY(!spectrogram->property("checked").toBool());

    // The switch moves the menu.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(spectrogramButton).center().toPoint());
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QVERIFY(spectrogram->property("checked").toBool());
    QVERIFY(!waveform->property("checked").toBool());

    // And the shortcuts, again and again.
    window->contentItem()->forceActiveFocus();
    for (int i = 0; i < 3; ++i) {
        QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
        QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
        QVERIFY(waveform->property("checked").toBool());
        QVERIFY(!spectrogram->property("checked").toBool());
        QTest::keyClick(window, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
        QVERIFY(spectrogram->property("checked").toBool());
        QVERIFY(!waveform->property("checked").toBool());
    }

    // The viewers repeat them.
    const QVariantList viewerActions = window->property("viewerActions").toList();
    QVERIFY(viewerActions.contains(QVariant::fromValue(spectrogram)));
    QVERIFY(viewerActions.contains(QVariant::fromValue(waveform)));
}

void UiMenusTest::scaleItemIsOffInWaveformMode() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *viewer = child(window, "viewerController");
    QObject *scale = child(window, "showScaleAction");
    QObject *meter = child(window, "showLevelMeterAction");
    QQuickItem *scaleItem = menuItemWithText(menuNamed(window, "viewMenu"), QStringLiteral("Show Frequency and Time Scale"));
    QVERIFY(scale != nullptr && meter != nullptr && scaleItem != nullptr);

    QVERIFY(scale->property("enabled").toBool());
    QVERIFY(scaleItem->isEnabled());
    QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1));
    QTRY_VERIFY(!scale->property("enabled").toBool());
    QVERIFY(!scaleItem->isEnabled());
    // The level meter is both plots'.
    QVERIFY(meter->property("enabled").toBool());
    QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0));
    QTRY_VERIFY(scale->property("enabled").toBool());

    // The checkboxes set the backend and follow it.
    QVERIFY(!scale->property("checked").toBool());
    QMetaObject::invokeMethod(scale, "trigger");
    QTRY_VERIFY(bridge->property("showSpectrogramScale").toBool());
    QVERIFY(scale->property("checked").toBool());
    bridge->setProperty("showSpectrogramScale", false);
    QTRY_VERIFY(!scale->property("checked").toBool());

    QVERIFY(!meter->property("checked").toBool());
    QMetaObject::invokeMethod(meter, "trigger");
    QTRY_VERIFY(bridge->property("showLevelMeter").toBool());
    QVERIFY(meter->property("checked").toBool());
    bridge->setProperty("showLevelMeter", false);
    QTRY_VERIFY(!meter->property("checked").toBool());
}

void UiMenusTest::editMenuNamesItsUndoAndClearsTheQueue() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *editMenu = menuNamed(window, "editMenu");
    QQuickItem *undo = menuItems(editMenu).value(0);
    QQuickItem *clear = menuItemWithText(editMenu, QStringLiteral("Clear Queue"));
    QVERIFY(undo != nullptr && clear != nullptr);

    QVERIFY(!undo->isEnabled());
    QVERIFY(!clear->isEnabled());
    bridge->setProperty("queueUndoId", 7.0);
    const QStringList names{QStringLiteral("Undo Clear Queue"), QStringLiteral("Undo Remove from Queue"),
                            QStringLiteral("Undo Replace Queue"), QStringLiteral("Undo Move in Queue")};
    for (qsizetype kind = 1; kind <= names.size(); ++kind) {
        bridge->setProperty("queueUndoKind", static_cast<int>(kind));
        QTRY_COMPARE(withoutMnemonic(undo->property("text").toString()), names.at(kind - 1));
    }
    QVERIFY(undo->isEnabled());
    QMetaObject::invokeMethod(qvariant_cast<QObject *>(undo->property("action")), "trigger");
    QTRY_VERIFY(bridge->property("queueCommands").toStringList().contains(QStringLiteral("undo 7")));

    bridge->setProperty("queueRows", sampleQueueRows(2));
    bridge->setProperty("queueLength", 2);
    QTRY_VERIFY(clear->isEnabled());
    QMetaObject::invokeMethod(qvariant_cast<QObject *>(clear->property("action")), "trigger");
    QTRY_VERIFY(bridge->property("queueCommands").toStringList().contains(QStringLiteral("clear")));
}

void UiMenusTest::findOpensSearchOnCtrlF() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *search = child(window, "globalSearchController");
    QVERIFY(search != nullptr);
    QVERIFY(!search->property("dialogVisible").toBool());

    window->contentItem()->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(search->property("dialogVisible").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!search->property("dialogVisible").toBool());

    // And from the Edit menu.
    QQuickItem *find = menuItemWithText(menuNamed(window, "editMenu"), QStringLiteral("Find…"));
    QVERIFY(find != nullptr);
    QMetaObject::invokeMethod(qvariant_cast<QObject *>(find->property("action")), "trigger");
    QTRY_VERIFY(search->property("dialogVisible").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!search->property("dialogVisible").toBool());
}

void UiMenusTest::shortcutsAreTakenOnceEach() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    QHash<QString, QString> owners;
    for (QObject *action : actionsOf(window)) {
        const QVariant shortcut = action->property("shortcut");
        QList<QKeySequence> sequences;
        if (shortcut.typeId() == QMetaType::Int) {
            sequences = QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(shortcut.toInt()));
        } else if (!shortcut.toString().isEmpty()) {
            sequences << QKeySequence(shortcut.toString());
        }
        for (const QKeySequence &sequence : std::as_const(sequences)) {
            const QString key = sequence.toString();
            const QString name = withoutMnemonic(action->property("text").toString());
            QVERIFY2(!owners.contains(key),
                     qPrintable(QStringLiteral("%1 is both %2's and %3's").arg(key, owners.value(key), name)));
            owners.insert(key, name);
        }
    }
    // Nothing takes Redo's keys until there is a redo.
    for (const QKeySequence &redo : QKeySequence::keyBindings(QKeySequence::Redo)) {
        QVERIFY2(!owners.contains(redo.toString()), qPrintable(redo.toString()));
    }
    // KDE's own: Ctrl+M shows the menu bar.
    QVERIFY(!owners.contains(QStringLiteral("Ctrl+M")));
    // The key hints of the context menus register nothing.
    for (QQuickItem *hint : window->findChildren<QQuickItem *>()) {
        if (hint->property("keyHint").isValid()) {
            QVERIFY(qvariant_cast<QObject *>(hint->property("action")) == nullptr);
        }
    }
}

void UiMenusTest::shortcutsTriggerTheirActions() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    bridge->setProperty("queueUndoId", 3.0);
    bridge->setProperty("queueUndoKind", 2);

    struct Press {
        const char *action;
        Qt::Key key;
        Qt::KeyboardModifiers modifiers;
    };
    const QList<Press> presses{
        {"playPauseAction", Qt::Key_Space, Qt::NoModifier},
        {"stopAction", Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier},
        {"previousAction", Qt::Key_Left, Qt::ControlModifier},
        {"nextAction", Qt::Key_Right, Qt::ControlModifier},
        {"volumeUpAction", Qt::Key_Up, Qt::AltModifier},
        {"volumeDownAction", Qt::Key_Down, Qt::AltModifier},
        {"muteAction", Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier},
        {"undoQueueChangeAction", Qt::Key_Z, Qt::ControlModifier},
        {"waveformModeAction", Qt::Key_2, Qt::ControlModifier},
        {"spectrogramModeAction", Qt::Key_1, Qt::ControlModifier},
        {"selectNextTrackAction", Qt::Key_Down, Qt::ControlModifier},
        {"selectPreviousTrackAction", Qt::Key_Up, Qt::ControlModifier},
        {"moveTrackDownAction", Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier},
        {"moveTrackUpAction", Qt::Key_Up, Qt::ControlModifier | Qt::ShiftModifier},
    };
    window->contentItem()->forceActiveFocus();
    for (const Press &press : presses) {
        QObject *action = child(window, press.action);
        QVERIFY2(action != nullptr, press.action);
        QVERIFY2(action->property("enabled").toBool(), press.action);
        QSignalSpy triggered(action, SIGNAL(triggered(QObject *)));
        // Twice: a key two actions shared would go to each in turn.
        QTest::keyClick(window, press.key, press.modifiers);
        QTest::keyClick(window, press.key, press.modifiers);
        QTRY_COMPARE_WITH_TIMEOUT(triggered.count(), 2, 1000);
    }
}

void UiMenusTest::queueContextMenuShowsKeysItDoesNotTake() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QTRY_VERIFY(viewRow(window, "playlistView", 1) != nullptr);

    rightClick(window, viewRow(window, "playlistView", 1));
    QObject *menu = nullptr;
    QTRY_VERIFY((menu = visibleMenu(window, QStringLiteral("playlistContextMenu"))) != nullptr);
    QTRY_VERIFY(menu->property("opened").toBool());
    QCOMPARE(rowsOf(menu),
             (QStringList{
                 QStringLiteral("hint|Play|Enter|media-playback-start"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Show in Library||go-jump"),
                 QStringLiteral("item|Open Containing Folder||folder-open"),
                 QStringLiteral("item|Edit Tags…||document-edit"),
                 QStringLiteral("sep"),
                 QStringLiteral("hint|Move Up|Ctrl+Shift+Up|go-up"),
                 QStringLiteral("hint|Move Down|Ctrl+Shift+Down|go-down"),
                 QStringLiteral("hint|Remove from Queue|Del|list-remove"),
             }));

    // The key sits where a shortcut does, clear of the item's text.
    QQuickItem *remove = menuItemWithText(menu, QStringLiteral("Remove from Queue"));
    QQuickItem *play = menuItemWithText(menu, QStringLiteral("Play"));
    QVERIFY(remove != nullptr && play != nullptr);
    QQuickItem *hint = itemsNamed(remove, QStringLiteral("menuKeyHint")).value(0);
    QQuickItem *label = textItem(remove->property("contentItem").value<QQuickItem *>(), QStringLiteral("Remove from Queue"));
    QVERIFY(hint != nullptr && label != nullptr);
    QVERIFY(hint->isVisible());
    QVERIFY(sceneRect(label).right() <= sceneRect(hint).left());
    QVERIFY(sceneRect(hint).right() <= sceneRect(remove).right());
    QVERIFY(sceneRect(remove).right() - sceneRect(hint).right() <= 12);
    // Every hint's right edge lines up.
    QQuickItem *playHint = itemsNamed(play, QStringLiteral("menuKeyHint")).value(0);
    QVERIFY(playHint != nullptr);
    QCOMPARE(sceneRect(playHint).right(), sceneRect(hint).right());
    // Not shortcuts: no action, so Enter and Delete stay the views'.
    QVERIFY(qvariant_cast<QObject *>(remove->property("action")) == nullptr);

    // Moving the first row up is not offered.
    QQuickItem *moveUp = menuItemWithText(menu, QStringLiteral("Move Up"));
    QVERIFY(moveUp->isEnabled());
    QMetaObject::invokeMethod(remove, "triggered");
    QTRY_VERIFY(bridge->property("queueCommands").toStringList().contains(QStringLiteral("remove 1")));
    QTRY_VERIFY(!menu->property("visible").toBool());

    rightClick(window, viewRow(window, "playlistView", 0));
    QTRY_VERIFY((menu = visibleMenu(window, QStringLiteral("playlistContextMenu"))) != nullptr);
    QVERIFY(!menuItemWithText(menu, QStringLiteral("Move Up"))->isEnabled());
    QVERIFY(menuItemWithText(menu, QStringLiteral("Move Down"))->isEnabled());
    QMetaObject::invokeMethod(menu, "close");
    QTRY_VERIFY(!menu->property("visible").toBool());
}

void UiMenusTest::libraryContextMenuNamesWhatItActsOn() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRootCount", 1);
    libraryModel.setExpandedKeys({kArtistKey, kAlbumKey});
    libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree());
    QTRY_VERIFY(viewRow(window, "libraryView", 2) != nullptr);

    const auto openOn = [&](int index) -> QObject * {
        QQuickItem *row = viewRow(window, "libraryView", index);
        if (row == nullptr) {
            return nullptr;
        }
        rightClick(window, row);
        QObject *menu = nullptr;
        if (!QTest::qWaitFor([&]() {
                menu = visibleMenu(window, QStringLiteral("libraryContextMenu"));
                return menu != nullptr && menu->property("opened").toBool();
            })) {
            return nullptr;
        }
        return menu;
    };
    const auto close = [&](QObject *menu) {
        QMetaObject::invokeMethod(menu, "close");
        return QTest::qWaitFor([&]() { return !menu->property("visible").toBool(); });
    };

    QObject *menu = openOn(1);
    QVERIFY(menu != nullptr);
    QCOMPARE(rowsOf(menu),
             (QStringList{
                 QStringLiteral("hint|Play Album|Enter|media-playback-start"),
                 QStringLiteral("item|Add Album to Queue||media-playlist-append"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Open Containing Folder||folder-open"),
                 QStringLiteral("item|Edit Tags…||document-edit"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Collapse All||collapse-all"),
             }));
    QVERIFY(close(menu));

    menu = openOn(2);
    QVERIFY(menu != nullptr);
    QCOMPARE(rowsOf(menu).mid(0, 2),
             (QStringList{QStringLiteral("hint|Play Track|Enter|media-playback-start"),
                          QStringLiteral("item|Add Track to Queue||media-playlist-append")}));
    QVERIFY(close(menu));

    // The artist has no tags of its own to edit.
    menu = openOn(0);
    QVERIFY(menu != nullptr);
    const QStringList artistRows = rowsOf(menu);
    QCOMPARE(artistRows.mid(0, 2),
             (QStringList{QStringLiteral("hint|Play Artist|Enter|media-playback-start"),
                          QStringLiteral("item|Add Artist to Queue||media-playlist-append")}));
    QVERIFY(!artistRows.contains(QStringLiteral("item|Edit Tags…||document-edit")));
    QVERIFY(close(menu));

    // On one of several selected rows, it acts on them all.
    QQuickItem *albumRow = viewRow(window, "libraryView", 1);
    QQuickItem *trackRow = viewRow(window, "libraryView", 2);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(albumRow).center().toPoint());
    QTest::mouseClick(window, Qt::LeftButton, Qt::ControlModifier, sceneRect(trackRow).center().toPoint());
    QTRY_COMPARE(libraryModel.property("selectionCount").toInt(), 2);
    menu = openOn(2);
    QVERIFY(menu != nullptr);
    QCOMPARE(rowsOf(menu).mid(0, 2),
             (QStringList{QStringLiteral("hint|Play Selection|Enter|media-playback-start"),
                          QStringLiteral("item|Add Selection to Queue||media-playlist-append")}));
    QVERIFY(close(menu));
}

void UiMenusTest::menusHaveNoTransitionsOfTheirOwn() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    // The style's: Breeze fades a menu in over Kirigami's short duration,
    // which Plasma's animation speed sets.
    QList<QObject *> menus = menusOf(window);
    menus << window->findChildren<QObject *>(QStringLiteral("playlistContextMenu"));
    menus << window->findChildren<QObject *>(QStringLiteral("libraryContextMenu"));
    for (QObject *menu : std::as_const(menus)) {
        auto *enter = qvariant_cast<QObject *>(menu->property("enter"));
        QVERIFY2(enter != nullptr, qPrintable(menu->objectName()));
        // Main.qml and the panes declare none: the style's are in its own file.
        const QString file = qmlContext(enter) != nullptr ? qmlContext(enter)->baseUrl().toString() : QString();
        QVERIFY2(file.contains(QStringLiteral("org/kde/desktop")), qPrintable(menu->objectName() + QLatin1Char(' ') + file));
    }
}

void UiMenusTest::settingsAndHelpOpenTheirPages() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    QObject *preferences = child(window, "preferencesDialog");
    QVERIFY(preferences != nullptr);
    QMetaObject::invokeMethod(child(window, "configureAction"), "trigger");
    QTRY_VERIFY(preferences->property("opened").toBool());
    QMetaObject::invokeMethod(preferences, "close");
    QTRY_VERIFY(!preferences->property("visible").toBool());

    // About KDE: KDE's icon and its words, with links.
    QObject *aboutKde = child(window, "aboutKdeDialog");
    QVERIFY(aboutKde != nullptr);
    QMetaObject::invokeMethod(child(window, "aboutKdeAction"), "trigger");
    QTRY_VERIFY(aboutKde->property("opened").toBool());
    QCOMPARE(aboutKde->property("title").toString(), QStringLiteral("About KDE"));
    auto *icon = aboutKde->findChild<QQuickItem *>(QStringLiteral("aboutKdeIcon"));
    auto *text = aboutKde->findChild<QQuickItem *>(QStringLiteral("aboutKdeText"));
    QVERIFY(icon != nullptr && text != nullptr);
    QCOMPARE(icon->property("source").toString(), QStringLiteral("kde"));
    QVERIFY(icon->property("valid").toBool());
    QVERIFY(text->property("text").toString().contains(QStringLiteral("href=\"https://kde.org/\"")));
    QTest::keyClick(aboutKde->property("contentItem").value<QQuickItem *>()->window(), Qt::Key_Escape);
    QTRY_VERIFY(!aboutKde->property("visible").toBool());

    // Report Bug… opens the project's issue form in the browser; the test
    // only reads where it goes.
    QCOMPARE(window->property("bugReportUrl").toString(),
             QStringLiteral("https://github.com/Tiaxi/ferrous/issues/new"));
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // The app's controls style, which it needs to start (docs/INSTALL.md).
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    // Settings, data and caches of the test's own.
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    // The same platform whatever the desktop the tests run in: without a
    // desktop, Qt loads no desktop's platform theme.
    qunsetenv("XDG_CURRENT_DESKTOP");
    qunsetenv("QT_QPA_PLATFORMTHEME");
    QApplication app(argc, argv);
    QIcon::setThemeName(QStringLiteral("breeze"));
    UiMenusTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_menus.moc"
