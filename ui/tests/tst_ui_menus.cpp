// SPDX-License-Identifier: GPL-3.0-or-later

// The menus of #373: the menu bar's menus with their words, icons, shortcuts
// and order; the Repeat and Visualization headings, which are text the
// pointer and the keyboard pass over; the radios, which follow the backend
// and the plot's mode switch; the items that turn off when they cannot act;
// the window's shortcuts, each taken once; the context menus of the
// queue and the library; and the menus as windows of their own, which
// reach past a small window (#480).

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFont>
#include <QKeySequence>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlListReference>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtMath>
#include <QtTest/QtTest>
#include <qqml.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <tuple>

#include "../src/DiagnosticsLogView.h"
#include "../src/InputModality.h"
#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/SpectrogramItem.h"
#include "../src/StandardKeys.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"
#include "../src/WindowPresence.h"
#include "TestColor.h"
#include "TestItems.h"
#include "TestMainWindow.h"
#include "TestMessages.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

// The window the tests load, at a size that shows every pane.
MainWindowOptions wideWindow() {
    MainWindowOptions options;
    options.width = 1600;
    options.height = 1000;
    return options;
}

QObject *bridgeOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiBridge"));
}

QObject *child(QQuickWindow *window, const char *name) {
    return window->findChild<QObject *>(QString::fromLatin1(name));
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

// The warnings captured so far that hold `text`, which it leaves for
// cleanup() to take.
QStringList capturedMessagesContaining(const QString &text) {
    QMutexLocker locker(&capturedMessageMutex());
    return capturedMessages().filter(text);
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
const QString kRootKey = QStringLiteral("root|/music");
const QString kAlbumKey = QStringLiteral("album|Artist A|Album A");

// An artist with an album of one track, in the bridge's tree format.
QByteArray artistAlbumTrackTree(bool underFolder = false) {
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
    QList<Row> rows{
        {1, 0, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, QStringLiteral("/music/Artist A"), {}},
        {2, 1, 0, 1, 2005, QStringLiteral("Album A"), kAlbumKey, QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackPath, kTrackPath, kTrackPath},
    };
    // Under a library folder's row, as with more than one folder.
    if (underFolder) {
        for (Row &row : rows) {
            ++row.depth;
        }
        rows.prepend({0, 0, 0, 1, 0, QStringLiteral("Shelf"), kRootKey, QStringLiteral("/music"), {}});
    }
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

// The menu bar's title at `index`, its delegate.
QQuickItem *menuBarItemAt(QObject *menuBar, int index) {
    QQuickItem *item = nullptr;
    QMetaObject::invokeMethod(menuBar, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, index));
    return item;
}

QQuickItem *childNamed(QQuickItem *root, const char *name) {
    const QList<QQuickItem *> found = itemsNamed(root, QString::fromLatin1(name));
    return found.isEmpty() ? nullptr : found.first();
}

// The colours of a Breeze scheme that UiPalette reads, from Breeze's
// .colors files: window, view, text, inactive text and accent.
struct Scheme {
    const char *window;
    const char *view;
    const char *text;
    const char *inactiveText;
    const char *highlight;
};

const Scheme kBreezeDark{"#202326", "#141618", "#fcfcfc", "#a1a9b1", "#3daee9"};
const Scheme kBreezeLight{"#eff0f1", "#ffffff", "#232629", "#707d8a", "#3daee9"};

// A window in `scheme` whose menu bar has Main.qml's titles, File and Edit.
QByteArray menuBarHarnessQml(const Scheme &scheme) {
    return QStringLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components

ApplicationWindow {
    id: window
    width: 600
    height: 300
    visible: true
    palette.window: "%1"
    palette.base: "%2"
    palette.text: "%3"
    palette.windowText: "%3"
    property alias tokens: tokens

    Item {
        Kirigami.Theme.inherit: false
        Kirigami.Theme.textColor: "%3"
        Kirigami.Theme.disabledTextColor: "%4"
        Kirigami.Theme.highlightColor: "%5"

        Components.UiPalette {
            id: tokens
            windowRoot: window
        }
    }

    menuBar: MenuBar {
        objectName: "harnessMenuBar"
        // The scheme's window colour, which Breeze's Header set has.
        Kirigami.Theme.backgroundColor: "%1"
        delegate: Components.AppMenuBarItem {
            uiPalette: tokens
        }
        Components.AppMenu {
            objectName: "harnessFileMenu"
            title: "&File"
            MenuItem { text: "&Open" }
        }
        Components.AppMenu {
            objectName: "harnessEditMenu"
            title: "&Edit"
            MenuItem { text: "&Undo" }
        }
    }
}
)QML")
        .arg(QLatin1String(scheme.window), QLatin1String(scheme.view), QLatin1String(scheme.text),
             QLatin1String(scheme.inactiveText), QLatin1String(scheme.highlight))
        .toUtf8();
}
// A window smaller than its menus: a File menu of twelve items with a
// submenu, a field that has the keyboard, and an area whose context menu
// opens where it is pressed.
// A standard key's action as the main window has it, its other bindings
// in the window, and a modal popup that repeats all of them, as a viewer
// does.
QByteArray standardKeyActionQml() {
    return QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "components" as Components

ApplicationWindow {
    width: 240
    height: 160
    visible: true
    property alias action: action
    property alias popup: popup

    Components.ShortcutAction {
        id: action
        standardKey: StandardKey.Open
    }
    Components.AlternateShortcuts {
        actions: [action]
    }
    Popup {
        id: popup
        modal: true
        width: 120
        height: 80
        Components.ActionShortcuts {
            actions: [action]
            active: popup.visible
        }
    }
}
)QML");
}

QByteArray smallWindowMenusQml() {
    return QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "components" as Components

ApplicationWindow {
    id: window
    width: 260
    height: 140
    visible: true
    property alias field: field
    property alias area: area
    property alias contextMenu: contextMenu

    menuBar: MenuBar {
        Components.AppMenu {
            objectName: "smallFileMenu"
            title: "&File"
            Repeater {
                model: 11
                MenuItem { text: "Item number " + (index + 1) }
            }
            Components.AppMenu {
                objectName: "smallRecentMenu"
                title: "&Recent"
                MenuItem { text: "&One" }
                MenuItem { text: "&Two" }
            }
        }
    }

    TextField {
        id: field
        width: 120
        focus: true
    }

    Item {
        id: area
        anchors.fill: parent
        anchors.topMargin: 40
    }

    Components.AppMenu {
        id: contextMenu
        objectName: "smallContextMenu"
        MenuItem { text: "&Copy" }
        MenuItem { text: "&Paste" }
    }
}
)QML");
}

// The window a menu's items are in.
QQuickWindow *menuWindowOf(QObject *menu) {
    auto *content = qvariant_cast<QQuickItem *>(menu->property("contentItem"));
    return content != nullptr ? content->window() : nullptr;
}

// A window's place on the screen.
QRect globalRect(QWindow *window) {
    return QRect(window->mapToGlobal(QPoint(0, 0)), window->size());
}
// A menu's window shows at the size the menu ends at, its items whole in
// it. On Wayland the compositor keeps the size a menu window is first shown
// at when the menu has asked for another since, and menus showed as empty
// strips a few pixels high (#646).
void verifyMenuShownWhole(QObject *menu, QQuickWindow *window, const PopupShowSizes &sizes) {
    const QString name = menu->objectName().isEmpty()
        ? menu->property("title").toString() + QLatin1Char(' ') + qmlContext(menu)->baseUrl().fileName()
        : menu->objectName();
    QTRY_VERIFY2(menu->property("opened").toBool(), qPrintable(name));
    QQuickWindow *menuWindow = menuWindowOf(menu);
    QVERIFY2(menuWindow != nullptr && menuWindow != window, qPrintable(name));
    QTRY_VERIFY2(menuWindow->isExposed(), qPrintable(name));
    auto *list = qvariant_cast<QQuickItem *>(menu->property("contentItem"));
    QVERIFY2(list != nullptr, qPrintable(name));
    QTRY_VERIFY2(list->property("contentHeight").toReal() > 0, qPrintable(name));
    const QSize shown = sizes.lastShown(menuWindow);
    const QSize now = menuWindow->size();
    QVERIFY2(shown == now, qPrintable(QStringLiteral("%1: shown at %2x%3, then %4x%5")
                                          .arg(name).arg(shown.width()).arg(shown.height())
                                          .arg(now.width()).arg(now.height())));
    QCOMPARE(now, QSize(qCeil(menu->property("implicitWidth").toReal()),
                        qCeil(menu->property("implicitHeight").toReal())));
    // The items, as laid out open, fill the window, with no scroll bar.
    const qreal itemsHeight = list->property("contentHeight").toReal() + menu->property("topPadding").toReal()
        + menu->property("bottomPadding").toReal();
    QVERIFY2(std::abs(itemsHeight - now.height()) <= 1.0,
             qPrintable(QStringLiteral("%1: items %2 high in a window %3 high").arg(name).arg(itemsHeight).arg(now.height())));
    QVERIFY2(!list->property("interactive").toBool(), qPrintable(name));
}
} // namespace

// Takes the URLs Qt.openUrlExternally would open, so no browser starts.
class UrlRecorder : public QObject {
    Q_OBJECT

public:
    QList<QUrl> urls;

public slots:
    void open(const QUrl &url) { urls.push_back(url); }
};

class UiMenusTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void menuBarTitleReadsWhenOpenOrHovered_data();
    void menuBarTitleReadsWhenOpenOrHovered();
    void menuBarTellsHoverFromOpen_data();
    void menuBarTellsHoverFromOpen();
    void menuItemGlyphsGrowWithTheFont_data();
    void menuItemGlyphsGrowWithTheFont();
    void everyMenuItemIsAnAppMenuItem();
    void marksShareTheGlyphColumn_data();
    void marksShareTheGlyphColumn();
    void markedGlyphsKeepTwoColumns();
    void showInLibraryShowsTheLibraryTree();
    void clearQueueShowsALineGlyph();
    void menusReadInKdesWordsAndOrder();
    void libraryFolderMenuNamesTheFolder();
    void menusAnswerThePointerAndTheKeyboard();
    void viewMenuFollowsThePlot();
    void queueAndPlaybackItemsActOnTheBackend();
    void menusAreWindowsThatReachPastASmallWindow();
    void menuWindowsShowAtTheirFinalSize();
    void standardKeyActionTakesEveryBindingUnderAModalPopup();
    void standardKeysReadAndWriteTheListsTheSettingsKeep();
    void openFolderNamesWhereItGoes();
    void playbackItemsAreOffWhereTheyCannotAct();
    void playbackItemsKeepTheirStateAsTheActionsChange();
    void everyMenuHeadingStartsItsGroup();

private:
    // The checks that the tests above run in turn in one window; each says
    // what it checks where it is defined.
    void checkMenuBarHasTheSixKdeMenusInOrder(QQuickWindow *window);
    void checkMenusListTheirItemsInOrder(QQuickWindow *window);
    void checkWordingIsTitleCaseWithOneCharacterEllipsis(MainWindowFixture &main);
    void checkShortcutsAreTakenOnceEach(QQuickWindow *window);
    void checkCoverMenusGiveReplaceFromItunesAnIcon(QQuickWindow *window);
    void checkMenusHaveNoTransitionsOfTheirOwn(QQuickWindow *window);
    void checkHeadingsAreTextThatIsNotPicked(QQuickWindow *window);
    void checkMenuBarTitlesAreLitWithTheSelectionTint(QQuickWindow *window);
    void checkHeadingsAreSkippedByThePointerAndTheKeyboard(QQuickWindow *window);
    void checkPlayPauseItemNamesWhatItDoes(QQuickWindow *window);
    void checkFindOpensSearchOnCtrlF(QQuickWindow *window);
    void checkStandardKeysTriggerTheirActionsByEveryBinding(QQuickWindow *window);
    void checkSettingsAndHelpOpenTheirPages(QQuickWindow *window);
    void checkAboutKdeLinksTakeTheKeyboard(QQuickWindow *window);
    void checkVisualizationRadiosFollowThePlotSwitch(QQuickWindow *window);
    void checkScaleItemIsOffInWaveformMode(QQuickWindow *window);
    void checkLevelMeterIsPartOfThePlot(QQuickWindow *window);
    void checkRepeatRadiosFollowTheBackendAndSetIt(QQuickWindow *window);
    void checkEditMenuNamesItsUndoAndRedoAndClearsTheQueue(QQuickWindow *window);
    void checkQueueContextMenuShowsKeysItDoesNotTake(QQuickWindow *window);
    void checkLibraryContextMenuNamesWhatItActsOn(MainWindowFixture &main);
    void checkMenusAreWindowsOfTheirOwn(QQuickWindow *window);
    void checkContextMenuOpensAtThePointer(QQuickWindow *window);
    void checkShortcutsTriggerTheirActions(QQuickWindow *window);
};

void UiMenusTest::initTestCase() {
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

void UiMenusTest::init() {
    clearCapturedMessages();
}

void UiMenusTest::cleanup() {
    dropCapturedMessages(offscreenSizeHintsWarning());
    const QString messages = takeCapturedMessagesText();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiMenusTest::cleanupTestCase() {
    stopCapturingMessages();
}

void UiMenusTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

// What the menus say and hold, as declared: the window's menus are those
// KDE's apps have, in KDE's words, and take each shortcut once.
void UiMenusTest::menusReadInKdesWordsAndOrder() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkMenuBarHasTheSixKdeMenusInOrder(window); },
               [&] { checkMenusListTheirItemsInOrder(window); },
               [&] { checkWordingIsTitleCaseWithOneCharacterEllipsis(main); },
               [&] { checkShortcutsAreTakenOnceEach(window); },
               [&] { checkCoverMenusGiveReplaceFromItunesAnIcon(window); },
               [&] { checkMenusHaveNoTransitionsOfTheirOwn(window); },
               [&] { checkHeadingsAreTextThatIsNotPicked(window); },
               [&] { checkMenusAreWindowsOfTheirOwn(window); }});
}

// A library folder's menu names it, as the other rows' menus name theirs
// (it said "Play" and "Add to Queue").
void UiMenusTest::libraryFolderMenuNamesTheFolder() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    bridgeOf(window)->setProperty("libraryRootCount", 2);
    main.libraryModel.setExpandedKeys({kRootKey});
    main.libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree(true));
    QTRY_VERIFY(viewRow(window, "libraryView", 0) != nullptr);
    rightClick(window, viewRow(window, "libraryView", 0));
    QObject *menu = nullptr;
    QTRY_VERIFY((menu = visibleMenu(window, QStringLiteral("libraryContextMenu"))) != nullptr
                && menu->property("opened").toBool());
    QCOMPARE(rowsOf(menu).mid(0, 2),
             (QStringList{QStringLiteral("hint|Play Folder|Enter|media-playback-start"),
                          QStringLiteral("item|Add Folder to Queue||media-playlist-append")}));
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    QTRY_VERIFY(!menu->property("visible").toBool());
}

// The menus under the pointer and the keyboard, and the pages and dialogs
// their items open. Each check leaves the menus closed.
void UiMenusTest::menusAnswerThePointerAndTheKeyboard() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkMenuBarTitlesAreLitWithTheSelectionTint(window); },
               [&] { checkHeadingsAreSkippedByThePointerAndTheKeyboard(window); },
               [&] { checkPlayPauseItemNamesWhatItDoes(window); },
               [&] { checkFindOpensSearchOnCtrlF(window); },
               [&] { checkStandardKeysTriggerTheirActionsByEveryBinding(window); },
               [&] { checkSettingsAndHelpOpenTheirPages(window); },
               [&] { checkAboutKdeLinksTakeTheKeyboard(window); }});
}

// The View menu's items and the plot they show. Each check leaves the
// spectrogram shown, without its scale or the level meter.
void UiMenusTest::viewMenuFollowsThePlot() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkVisualizationRadiosFollowThePlotSwitch(window); },
               [&] { checkScaleItemIsOffInWaveformMode(window); },
               [&] { checkLevelMeterIsPartOfThePlot(window); }});
}

// The items that act on the queue and on playback, and the backend's state
// they follow. Each check sets the queue it needs.
void UiMenusTest::queueAndPlaybackItemsActOnTheBackend() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    runInTurn({[&] { checkRepeatRadiosFollowTheBackendAndSetIt(window); },
               [&] { checkEditMenuNamesItsUndoAndRedoAndClearsTheQueue(window); },
               [&] { checkQueueContextMenuShowsKeysItDoesNotTake(window); },
               [&] { checkContextMenuOpensAtThePointer(window); },
               [&] { checkLibraryContextMenuNamesWhatItActsOn(main); },
               [&] { checkShortcutsTriggerTheirActions(window); }});
}

// A menu is a window of its own, so a window smaller than the menu does not
// cut it off or squeeze it: it reaches past the window's bottom, its
// submenu opens beside it as another window, and a context menu opened near
// the window's corner reaches past its side, from the point it was opened
// at. The field has the keyboard again once a menu closes.
//
// Offscreen, Qt places the menus. On Wayland the compositor does, from the
// item each menu opens beside; it opens a menu window only for input it
// sent, which QTest's is not, so that was checked by hand under KWin.
void UiMenusTest::menusAreWindowsThatReachPastASmallWindow() {
    movePointerOffTheWindows();
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(smallWindowMenusQml(),
                      QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/SmallWindowMenus.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    auto *field = qvariant_cast<QQuickItem *>(root->property("field"));
    auto *area = qvariant_cast<QQuickItem *>(root->property("area"));
    QObject *contextMenu = qvariant_cast<QObject *>(root->property("contextMenu"));
    QObject *fileMenu = root->findChild<QObject *>(QStringLiteral("smallFileMenu"));
    QObject *recentMenu = root->findChild<QObject *>(QStringLiteral("smallRecentMenu"));
    QVERIFY(field != nullptr && area != nullptr && contextMenu != nullptr && fileMenu != nullptr
            && recentMenu != nullptr);
    QTRY_VERIFY(field->hasActiveFocus());
    const QRect windowRect = globalRect(window);

    // The menu bar's menu: a window over the main one, from the title down,
    // the whole of its items past the window's bottom.
    QQuickItem *file = menuBarItemAt(qvariant_cast<QObject *>(root->property("menuBar")), 0);
    QVERIFY(file != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(file).center().toPoint());
    QTRY_VERIFY(fileMenu->property("opened").toBool());
    QQuickWindow *fileWindow = menuWindowOf(fileMenu);
    QVERIFY(fileWindow != nullptr && fileWindow != window);
    QCOMPARE(fileWindow->type(), Qt::Popup);
    QCOMPARE(fileWindow->transientParent(), window);
    QTRY_VERIFY(fileWindow->isExposed());
    const QPoint fileAt = window->mapToGlobal(sceneRect(file).bottomLeft().toPoint());
    QTRY_VERIFY2(std::abs(globalRect(fileWindow).left() - fileAt.x()) <= 2
                     && std::abs(globalRect(fileWindow).top() - fileAt.y()) <= 2,
                 qPrintable(QStringLiteral("menu at %1,%2, title's corner at %3,%4")
                                .arg(globalRect(fileWindow).left()).arg(globalRect(fileWindow).top())
                                .arg(fileAt.x()).arg(fileAt.y())));
    QVERIFY2(globalRect(fileWindow).bottom() > windowRect.bottom() + 100,
             qPrintable(QStringLiteral("menu's bottom %1, window's %2")
                            .arg(globalRect(fileWindow).bottom()).arg(windowRect.bottom())));
    QVERIFY(fileWindow->height() >= qCeil(fileMenu->property("implicitHeight").toReal()));

    // Its submenu: another window, beside it, from the keyboard.
    QQuickItem *recent = menuItems(fileMenu).value(11);
    QVERIFY(recent != nullptr);
    fileMenu->setProperty("currentIndex", 11);
    QTRY_VERIFY(recent->hasActiveFocus());
    QTest::keyClick(fileWindow, Qt::Key_Right);
    QTRY_VERIFY(recentMenu->property("opened").toBool());
    QQuickWindow *recentWindow = menuWindowOf(recentMenu);
    QVERIFY(recentWindow != nullptr && recentWindow != fileWindow && recentWindow != window);
    QTRY_VERIFY(recentWindow->isExposed());
    QTRY_VERIFY2(globalRect(recentWindow).left() >= globalRect(fileWindow).right() - 8,
                 qPrintable(QStringLiteral("submenu's left %1, menu's right %2")
                                .arg(globalRect(recentWindow).left()).arg(globalRect(fileWindow).right())));
    QTest::keyClick(recentWindow, Qt::Key_Escape);
    QTRY_VERIFY(!recentMenu->property("visible").toBool());
    QTRY_VERIFY(fileMenu->property("opened").toBool());
    QTest::keyClick(fileWindow, Qt::Key_Escape);
    QTRY_VERIFY(!fileMenu->property("visible").toBool());
    QTRY_VERIFY(field->hasActiveFocus());

    // A context menu near the window's corner: at the point, past the
    // window's side. Its parent is an item a pixel in size at the point,
    // hidden, in the item it opened for, which the compositor opens it
    // beside.
    const QPointF point(area->width() - 12, area->height() - 12);
    QVERIFY(QMetaObject::invokeMethod(contextMenu, "popupAt", Q_ARG(QVariant, QVariant::fromValue(area)),
                                      Q_ARG(QVariant, point.x()), Q_ARG(QVariant, point.y())));
    QTRY_VERIFY(contextMenu->property("opened").toBool());
    auto *anchor = qvariant_cast<QQuickItem *>(contextMenu->property("parent"));
    QVERIFY(anchor != nullptr);
    QCOMPARE(anchor->objectName(), QStringLiteral("appMenuPointAnchor"));
    QCOMPARE(anchor->parentItem(), area);
    QCOMPARE(QPointF(anchor->x(), anchor->y()), point);
    QCOMPARE(QSizeF(anchor->width(), anchor->height()), QSizeF(1, 1));
    QVERIFY(!anchor->isVisible());
    QQuickWindow *contextWindow = menuWindowOf(contextMenu);
    QVERIFY(contextWindow != nullptr && contextWindow != window);
    QTRY_VERIFY(contextWindow->isExposed());
    const QPoint pointAt = window->mapToGlobal(area->mapToScene(point).toPoint());
    QTRY_VERIFY2(std::abs(globalRect(contextWindow).left() - pointAt.x()) <= 2
                     && std::abs(globalRect(contextWindow).top() - pointAt.y()) <= 2,
                 qPrintable(QStringLiteral("menu at %1,%2, point at %3,%4")
                                .arg(globalRect(contextWindow).left()).arg(globalRect(contextWindow).top())
                                .arg(pointAt.x()).arg(pointAt.y())));
    QVERIFY(globalRect(contextWindow).right() > windowRect.right());
    QVERIFY(globalRect(contextWindow).bottom() > windowRect.bottom());
    QTest::keyClick(contextWindow, Qt::Key_Escape);
    QTRY_VERIFY(!contextMenu->property("visible").toBool());
    QTRY_VERIFY(field->hasActiveFocus());
}

// Each menu's window shows at the size the menu ends at, its items whole in
// it, the first time it opens and every time after: a context menu first
// opened in a fresh window, the menu bar's menus as the pointer moves from
// title to title with one open, and every other menu of the window (#646).
// Closed, Breeze's menu items are 0 high, and the menu was a strip a few
// pixels high; Qt shows a menu's window at its closed size.
void UiMenusTest::menuWindowsShowAtTheirFinalSize() {
    PopupShowSizes sizes;
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    // The queue's context menu, the window's first menu.
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRow(window, "playlistView", 1)) != nullptr);
    QObject *queueMenu = window->findChild<QObject *>(QStringLiteral("playlistContextMenu"));
    QVERIFY(queueMenu != nullptr);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      (sceneRect(row).topLeft() + QPointF(60, row->height() / 2)).toPoint());
    verifyMenuShownWhole(queueMenu, window, sizes);
    if (QTest::currentTestFailed()) {
        return;
    }
    QTest::keyClick(menuWindowOf(queueMenu), Qt::Key_Escape);
    QTRY_VERIFY(!queueMenu->property("visible").toBool());

    // The menu bar: File opened with a click, then each title hovered in
    // turn, which opens its menu in place of the last, and back to File.
    const QList<QObject *> menus = menusOf(window);
    QVERIFY(menus.size() >= 2);
    QObject *menuBar = menuBarOf(window);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(menuBarItemAt(menuBar, 0)).center().toPoint());
    verifyMenuShownWhole(menus.first(), window, sizes);
    if (QTest::currentTestFailed()) {
        return;
    }
    QList<int> order;
    for (int i = 1; i < menus.size(); ++i) {
        order << i;
    }
    order << 0;
    int last = 0;
    for (int index : order) {
        QTest::mouseMove(window, sceneRect(menuBarItemAt(menuBar, index)).center().toPoint());
        verifyMenuShownWhole(menus.at(index), window, sizes);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY(!menus.at(last)->property("visible").toBool());
        last = index;
    }
    QTest::keyClick(menuWindowOf(menus.first()), Qt::Key_Escape);
    QTRY_VERIFY(!menus.first()->property("visible").toBool());

    // Every other menu of the window that is a window of its own, opened
    // over the item it opens beside.
    const QString ours = QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/")).toString();
    constexpr int windowPopup = 1;
    int opened = 0;
    for (QObject *menu : window->findChildren<QObject *>()) {
        const QQmlContext *context = qmlContext(menu);
        if (!menu->inherits("QQuickMenu") || context == nullptr || !context->baseUrl().toString().startsWith(ours)
            || menu->property("popupType").toInt() != windowPopup || menus.contains(menu) || menu == queueMenu) {
            continue;
        }
        auto *parent = qvariant_cast<QQuickItem *>(menu->property("parent"));
        // Those of dialogs, which are closed, are left out.
        if (parent == nullptr || !parent->isVisible() || parent->window() != window) {
            continue;
        }
        QMetaObject::invokeMethod(menu, "open");
        verifyMenuShownWhole(menu, window, sizes);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTest::keyClick(menuWindowOf(menu), Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
        ++opened;
    }
    QVERIFY(opened > 0);
    // Opening them warned of nothing, in the style the app sets: Breeze's
    // menu item, shown an action's standard key, took only its first binding
    // and warned of the rest, as of Open's Open key (#581).
    QCOMPARE(QQuickStyle::name(), QStringLiteral("org.kde.desktop"));
    const QStringList shortcutWarnings = capturedMessagesContaining(QStringLiteral("Shortcut:"));
    QVERIFY2(shortcutWarnings.isEmpty(), qPrintable(shortcutWarnings.join(QLatin1Char('\n'))));
}

// A viewer's modal popup blocks the window's shortcuts and repeats them, a
// standard key's with every binding, as the window has them (#581). On
// Plasma, Quit has more than one; offscreen, Open does.
void UiMenusTest::standardKeyActionTakesEveryBindingUnderAModalPopup() {
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(standardKeyActionQml(),
                      QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/ShortcutAction.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    QObject *action = qvariant_cast<QObject *>(root->property("action"));
    QObject *popup = qvariant_cast<QObject *>(root->property("popup"));
    QVERIFY(action != nullptr && popup != nullptr);
    const QList<QKeySequence> bindings = QKeySequence::keyBindings(QKeySequence::Open);
    QVERIFY(bindings.size() > 1);

    QSignalSpy triggered(action, SIGNAL(triggered(QObject *)));
    for (const bool underPopup : {false, true}) {
        if (underPopup) {
            QMetaObject::invokeMethod(popup, "open");
            QTRY_VERIFY(popup->property("opened").toBool());
        }
        for (const QKeySequence &binding : bindings) {
            const int before = triggered.count();
            QTest::keySequence(window, binding);
            // Once: a key two shortcuts shared would trigger neither.
            QTRY_VERIFY2(triggered.count() == before + 1,
                         qPrintable(binding.toString() + (underPopup ? QStringLiteral(" under the popup") : QString())));
        }
    }
    QCOMPARE(triggered.count(), 2 * bindings.size());
    // QTest's, as it gives the Open key no text.
    dropCapturedMessagesMatching(QRegularExpression(QStringLiteral("^keyToAscii Unhandled key code Qt::Key_\\w+$")));
}

// Each of the window's menus is a window of its own but the album art
// viewer's, which moves between the viewer's full-screen window and the
// main window (see AlbumArtSurface.qml).
void UiMenusTest::checkMenusAreWindowsOfTheirOwn(QQuickWindow *window) {
    constexpr int itemPopup = 0;
    constexpr int windowPopup = 1;
    QStringList found;
    const QString ours = QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/")).toString();
    for (QObject *menu : window->findChildren<QObject *>()) {
        const QQmlContext *context = qmlContext(menu);
        // Kirigami's own, such as a link's Copy Link, are left as they are.
        if (!menu->inherits("QQuickMenu") || context == nullptr || !context->baseUrl().toString().startsWith(ours)) {
            continue;
        }
        const bool viewer = menu->objectName() == QLatin1String("albumArtViewerContextMenu");
        // A window menu has no fade out (see AppMenu.qml); the viewer's,
        // an item, keeps the style's.
        QVERIFY2((qvariant_cast<QObject *>(menu->property("exit")) == nullptr) != viewer,
                 qPrintable(menu->objectName()));
        QVERIFY2(menu->property("popupType").toInt() == (viewer ? itemPopup : windowPopup),
                 qPrintable(QString::fromLatin1(menu->metaObject()->className()) + QLatin1Char(' ')
                            + menu->objectName() + QLatin1Char(' ')
                            + context->baseUrl().fileName()));
        found << menu->objectName();
    }
    for (const char *name : {"fileMenu", "playbackMenu", "playlistContextMenu", "libraryContextMenu",
                             "albumArtMenu", "scrobbleIndicatorMenu", "albumArtViewerContextMenu"}) {
        QVERIFY2(found.contains(QLatin1String(name)), name);
    }

    // Each of the menu bar's menus opens as one, over the main window, and
    // its window goes as it closes: on Wayland a menu opened while another
    // menu's window still shows is made its child, and goes with it.
    for (QObject *menu : menusOf(window)) {
        QMetaObject::invokeMethod(menu, "open");
        QTRY_VERIFY(menu->property("opened").toBool());
        QQuickWindow *menuWindow = menuWindowOf(menu);
        QVERIFY2(menuWindow != nullptr && menuWindow != window, qPrintable(menu->objectName()));
        QCOMPARE(menuWindow->type(), Qt::Popup);
        QCOMPARE(menuWindow->transientParent(), window);
        QTRY_VERIFY(menuWindow->isVisible());
        QTest::keyClick(menuWindow, Qt::Key_Escape);
        QVERIFY2(!menuWindow->isVisible(), qPrintable(menu->objectName()));
        QVERIFY(!menu->property("visible").toBool());
    }
}

// The queue's context menu opens where the right button pressed, on the
// row, not at the row's or the list's corner.
void UiMenusTest::checkContextMenuOpensAtThePointer(QQuickWindow *window) {
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRow(window, "playlistView", 1)) != nullptr);
    // Its items are as wide closed as open: the menu's window takes the
    // closed menu's size when it is made (see KeyHintMenuItem.qml).
    QObject *menu = window->findChild<QObject *>(QStringLiteral("playlistContextMenu"));
    QVERIFY(menu != nullptr && !menu->property("visible").toBool());
    QList<qreal> closedWidths;
    for (QQuickItem *item : menuItems(menu)) {
        closedWidths << item->implicitWidth();
    }
    const QPoint at = (sceneRect(row).topLeft() + QPointF(60, row->height() / 2)).toPoint();
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, at);
    QTRY_VERIFY(menu->property("opened").toBool());
    QList<qreal> openWidths;
    for (QQuickItem *item : menuItems(menu)) {
        openWidths << item->implicitWidth();
    }
    QCOMPARE(openWidths, closedWidths);
    auto *anchor = qvariant_cast<QQuickItem *>(menu->property("parent"));
    QVERIFY(anchor != nullptr);
    QCOMPARE(anchor->objectName(), QStringLiteral("appMenuPointAnchor"));
    QCOMPARE(anchor->mapToScene(QPointF(0, 0)).toPoint(), at);
    QVERIFY(row->isAncestorOf(anchor));
    QQuickWindow *menuWindow = menuWindowOf(menu);
    QVERIFY(menuWindow != nullptr && menuWindow != window);
    const QPoint pointAt = window->mapToGlobal(at);
    QTRY_VERIFY2(std::abs(globalRect(menuWindow).left() - pointAt.x()) <= 2
                     && std::abs(globalRect(menuWindow).top() - pointAt.y()) <= 2,
                 qPrintable(QStringLiteral("menu at %1,%2, pressed at %3,%4")
                                .arg(globalRect(menuWindow).left()).arg(globalRect(menuWindow).top())
                                .arg(pointAt.x()).arg(pointAt.y())));
    QTest::keyClick(menuWindow, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
}

void UiMenusTest::checkMenuBarHasTheSixKdeMenusInOrder(QQuickWindow *window) {

    QStringList titles;
    for (QObject *menu : menusOf(window)) {
        titles << menu->property("title").toString();
    }
    QCOMPARE(titles, (QStringList{QStringLiteral("&File"), QStringLiteral("&Edit"), QStringLiteral("&View"),
                                  QStringLiteral("&Playback"), QStringLiteral("&Settings"),
                                  QStringLiteral("&Help")}));
}

// Main.qml's titles are the app's: plain, they show the bar; open, the
// selection tint of the lists, with the text colour on both.
void UiMenusTest::checkMenuBarTitlesAreLitWithTheSelectionTint(QQuickWindow *window) {
    auto *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QObject *menuBar = menuBarOf(window);
    QVERIFY(palette != nullptr && menuBar != nullptr);
    const QColor text = palette->property("text").value<QColor>();
    const QColor selection = palette->property("selection").value<QColor>();

    // Fresh, with the pointer off the window, no title is lit.
    QVERIFY(!window->geometry().contains(QCursor::pos()));
    const QList<QObject *> menus = menusOf(window);
    QCOMPARE(menuBar->property("count").toInt(), menus.size());
    for (int i = 0; i < menus.size(); ++i) {
        QQuickItem *title = menuBarItemAt(menuBar, i);
        QVERIFY(title != nullptr);
        const QString name = withoutMnemonic(menus.at(i)->property("title").toString());
        QQuickItem *label = childNamed(title, "menuBarItemLabel");
        QQuickItem *background = childNamed(title, "menuBarItemBackground");
        QVERIFY2(label != nullptr && background != nullptr, qPrintable(name));
        QCOMPARE(label->property("color").value<QColor>(), text);
        QVERIFY2(!background->isVisible(), qPrintable(name));
    }

    // Opened, View is lit; closed, it is plain again. Escape can leave the
    // keyboard on the bar, with View the current title and lit; a second
    // Escape leaves the bar.
    QQuickItem *view = menuBarItemAt(menuBar, 2);
    QQuickItem *viewBackground = childNamed(view, "menuBarItemBackground");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(view).center().toPoint());
    QTRY_VERIFY(menus.at(2)->property("visible").toBool());
    QTRY_COMPARE(viewBackground->opacity(), 1.0);
    QCOMPARE(viewBackground->property("color").value<QColor>(), selection);
    QTest::mouseMove(window, QPoint(window->width() / 2, window->height() / 2));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!menus.at(2)->property("visible").toBool());
    if (view->property("highlighted").toBool()) {
        QTRY_COMPARE(viewBackground->opacity(), 1.0);
        QTest::keyClick(window, Qt::Key_Escape);
    }
    QTRY_VERIFY(!view->property("highlighted").toBool());
    QTRY_VERIFY(!viewBackground->isVisible());
}

void UiMenusTest::menuBarTitleReadsWhenOpenOrHovered_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

// The open title is the text colour on the selection tint, as in the
// lists, and well over 4.5:1 in both themes (the style's white on
// a translucent accent was 2.08:1 in light). It keeps the style's
// size, padding and Alt mnemonics, and Alt+letter opens its menu.
void UiMenusTest::menuBarTitleReadsWhenOpenOrHovered() {
    QFETCH(bool, dark);
    const Scheme &scheme = dark ? kBreezeDark : kBreezeLight;
    movePointerOffTheWindows();
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(menuBarHarnessQml(scheme),
                      QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/MenuBarHarness.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    auto *tokens = qvariant_cast<QObject *>(root->property("tokens"));
    QObject *menuBar = qvariant_cast<QObject *>(root->property("menuBar"));
    QVERIFY(tokens != nullptr && menuBar != nullptr);
    QObject *fileMenu = root->findChild<QObject *>(QStringLiteral("harnessFileMenu"));
    QQuickItem *file = menuBarItemAt(menuBar, 0);
    QQuickItem *edit = menuBarItemAt(menuBar, 1);
    QVERIFY(fileMenu != nullptr && file != nullptr && edit != nullptr);
    QQuickItem *label = childNamed(file, "menuBarItemLabel");
    QQuickItem *background = childNamed(file, "menuBarItemBackground");
    QVERIFY(label != nullptr && background != nullptr);

    // The declared colours: the text on the selection tint.
    const QColor text = tokens->property("text").value<QColor>();
    const QColor selection = tokens->property("selection").value<QColor>();
    QCOMPARE(text, QColor(QLatin1String(scheme.text)));
    QCOMPARE(selection.alpha(), 255);
    const double ratio = contrastRatio(text, selection);
    QVERIFY2(ratio >= 4.5, qPrintable(QStringLiteral("%1 on %2 is %3:1")
                                          .arg(text.name(), selection.name()).arg(ratio, 0, 'f', 2)));
    QVERIFY2(ratio >= (dark ? 9.5 : 10.0), qPrintable(QString::number(ratio)));

    // The style's size and padding.
    QCOMPARE(file->property("leftPadding").toReal(), file->property("rightPadding").toReal());
    QVERIFY(file->property("leftPadding").toReal() > 0.0);
    QCOMPARE(file->property("topPadding").toReal(), file->property("bottomPadding").toReal());
    QVERIFY(file->property("topPadding").toReal() > 0.0);
    QCOMPARE(file->width(), std::max(file->property("implicitBackgroundWidth").toReal(),
                                     file->property("implicitContentWidth").toReal()
                                         + file->property("leftPadding").toReal()
                                         + file->property("rightPadding").toReal()));

    // Plain, with the pointer off the window: the bar shows through, and
    // the text is the text colour.
    QVERIFY(!window->geometry().contains(QCursor::pos()));
    QVERIFY(!background->isVisible());
    QCOMPARE(label->property("color").value<QColor>(), text);

    // Hovered: lit with the chrome's hover tint (see
    // menuBarTellsHoverFromOpen).
    QTest::mouseMove(window, sceneRect(file).center().toPoint());
    QTRY_COMPARE(background->opacity(), 1.0);
    QCOMPARE(label->property("color").value<QColor>(), text);
    QVERIFY(!childNamed(edit, "menuBarItemBackground")->isVisible());

    // Open: lit, and drawn so.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(file).center().toPoint());
    QTRY_VERIFY(fileMenu->property("visible").toBool());
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 10));
    QTRY_COMPARE(background->opacity(), 1.0);
    QCOMPARE(label->property("color").value<QColor>(), text);
    const QImage image = window->grabWindow();
    const qreal scale = static_cast<qreal>(image.width()) / window->width();
    const QPointF corner = sceneRect(file).topLeft() + QPointF(2, 2);
    const QColor drawn = image.pixelColor(static_cast<int>(corner.x() * scale), static_cast<int>(corner.y() * scale));
    QVERIFY2(channelDistance(drawn, selection) <= 1,
             qPrintable(QStringLiteral("drawn %1, selection %2").arg(drawn.name(), selection.name())));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!fileMenu->property("visible").toBool());
    QTRY_VERIFY(!background->isVisible());

    // Alt underlines the mnemonics, and Alt+F opens File.
    QVERIFY(!label->property("text").toString().contains(QStringLiteral("<u>")));
    QTest::keyPress(window, Qt::Key_Alt, Qt::AltModifier);
    QTRY_VERIFY2(label->property("text").toString().contains(QStringLiteral("<u>F</u>")),
                 qPrintable(label->property("text").toString()));
    QTest::keyClick(window, Qt::Key_F, Qt::AltModifier);
    QTest::keyRelease(window, Qt::Key_Alt, Qt::NoModifier);
    QTRY_VERIFY(fileMenu->property("visible").toBool());
    QTRY_COMPARE(background->opacity(), 1.0);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!fileMenu->property("visible").toBool());
}

void UiMenusTest::menuBarTellsHoverFromOpen_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

// A title the pointer only passes over, with no menu open, takes the
// chrome's neutral hover tint; an open title, or the current one the
// keyboard is on, the selection tint. The two read apart as drawn, and
// the hover still stands out from the bar.
void UiMenusTest::menuBarTellsHoverFromOpen() {
    QFETCH(bool, dark);
    const Scheme &scheme = dark ? kBreezeDark : kBreezeLight;
    movePointerOffTheWindows();
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(menuBarHarnessQml(scheme),
                      QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/MenuBarHarness.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    auto *tokens = qvariant_cast<QObject *>(root->property("tokens"));
    QObject *menuBar = qvariant_cast<QObject *>(root->property("menuBar"));
    QVERIFY(tokens != nullptr && menuBar != nullptr);
    QObject *fileMenu = root->findChild<QObject *>(QStringLiteral("harnessFileMenu"));
    QQuickItem *file = menuBarItemAt(menuBar, 0);
    QVERIFY(fileMenu != nullptr && file != nullptr);
    QQuickItem *background = childNamed(file, "menuBarItemBackground");
    QVERIFY(background != nullptr);

    const QColor selection = tokens->property("selection").value<QColor>();
    const QColor chrome = tokens->property("chrome").value<QColor>();
    QVariant hoverTint;
    QVERIFY(QMetaObject::invokeMethod(tokens, "hoverOn", Q_RETURN_ARG(QVariant, hoverTint), Q_ARG(QVariant, chrome)));
    const QColor hover = hoverTint.value<QColor>();
    QVERIFY(hover.isValid() && hover.alpha() < 255);

    // A pixel of the title's background, inside its corner, as drawn.
    const auto drawnTitle = [&]() {
        const QImage image = window->grabWindow();
        const qreal scale = static_cast<qreal>(image.width()) / window->width();
        const QPointF corner = sceneRect(file).topLeft() + QPointF(2, 2);
        return image.pixelColor(static_cast<int>(corner.x() * scale), static_cast<int>(corner.y() * scale));
    };
    const QColor bar = drawnTitle();

    // Hovered, no menu open: the hover tint, though the bar makes the
    // title its current one.
    QTest::mouseMove(window, sceneRect(file).center().toPoint());
    QTRY_VERIFY(file->property("hovered").toBool());
    QVERIFY(file->property("highlighted").toBool());
    QTRY_COMPARE(background->opacity(), 1.0);
    QCOMPARE(background->property("color").value<QColor>(), hover);
    QVERIFY(!file->property("selected").toBool());
    const QColor hovered = drawnTitle();
    QVERIFY2(channelDistance(hovered, drawnOver(hover, bar)) <= 2,
             qPrintable(QStringLiteral("drawn %1, hover %2 over %3")
                            .arg(hovered.name(), drawnOver(hover, bar).name(), bar.name())));
    QVERIFY2(contrastRatio(hovered, bar) >= 1.2,
             qPrintable(QStringLiteral("hover %1 on the bar %2").arg(hovered.name(), bar.name())));

    // Passed over: plain, though the bar may keep it current until the
    // pointer leaves the bar (offscreen, the bar has no hover events).
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 10));
    QTRY_VERIFY(!file->property("hovered").toBool());
    QTRY_VERIFY(!background->isVisible());

    // Open, under the pointer: the selection tint, which reads apart from
    // the hover.
    QTest::mouseMove(window, sceneRect(file).center().toPoint());
    QTRY_VERIFY(file->property("hovered").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(file).center().toPoint());
    QTRY_VERIFY(fileMenu->property("visible").toBool());
    QVERIFY(file->property("hovered").toBool());
    QTRY_COMPARE(background->property("color").value<QColor>(), selection);
    QTRY_COMPARE(background->opacity(), 1.0);
    const QColor open = drawnTitle();
    QVERIFY2(channelDistance(open, selection) <= 1,
             qPrintable(QStringLiteral("drawn %1, selection %2").arg(open.name(), selection.name())));
    QVERIFY2(channelDistance(open, hovered) >= 20,
             qPrintable(QStringLiteral("open %1, hovered %2").arg(open.name(), hovered.name())));

    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!fileMenu->property("visible").toBool());

    // The keyboard on the bar, as after Alt, with no menu open: the
    // selection tint, the pointer off the bar.
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 10));
    QTRY_VERIFY(!file->property("hovered").toBool());
    QTRY_VERIFY(!background->isVisible());
    QMetaObject::invokeMethod(menuBar, "forceActiveFocus");
    QTRY_VERIFY(menuBar->property("activeFocus").toBool());
    if (file->property("highlighted").toBool()) {
        // Still the bar's current title from the pointer: Right and Left
        // come back to it.
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_Left);
    } else {
        QTest::keyClick(window, Qt::Key_Right);
    }
    QTRY_VERIFY(file->property("highlighted").toBool());
    QVERIFY(!fileMenu->property("visible").toBool());
    QTRY_VERIFY(file->property("keyboardCurrent").toBool());
    QTRY_COMPARE(background->opacity(), 1.0);
    QCOMPARE(background->property("color").value<QColor>(), selection);

    // Escape leaves the bar: plain.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!file->property("highlighted").toBool());
    QTRY_VERIFY(!background->isVisible());
}

void UiMenusTest::menuItemGlyphsGrowWithTheFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("glyph");
    QTest::addColumn<int>("gap");
    QTest::newRow("10 pt") << 10.0 << 16 << 5;
    QTest::newRow("24 pt") << 24.0 << 32 << 8;
    QTest::newRow("40 pt") << 40.0 << 32 << 8;
}

// A menu item's glyph grows with the font as its words do, from 16 px at
// the default font to 32 px and no larger, in the menu bar's menus and the
// context menus, and the gap to the words grows with it: the row's
// spacing at the default font, a quarter of the glyph at 32 px. At the
// row's spacing, Previous and Next crowded their words at 24 pt.
void UiMenusTest::menuItemGlyphsGrowWithTheFont() {
    QFETCH(qreal, pointSize);
    QFETCH(int, glyph);
    QFETCH(int, gap);
    ScopedApplicationFont font(pointSize);
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    // Every item of every menu bar menu, as declared.
    for (QObject *menu : menusOf(window)) {
        for (QQuickItem *item : menuItems(menu)) {
            if (item == nullptr || item->inherits("QQuickMenuSeparator")) {
                continue;
            }
            const QString name = withoutMnemonic(item->property("text").toString());
            QCOMPARE(QQmlProperty::read(item, QStringLiteral("icon.width")).toInt(), glyph);
            QCOMPARE(QQmlProperty::read(item, QStringLiteral("icon.height")).toInt(), glyph);
            QVERIFY2(childNamed(item, "appMenuItemIcon") != nullptr, qPrintable(name));
        }
    }

    // As drawn: the glyph at its size, the words a spacing after it.
    const auto checkDrawn = [&](QObject *menu) {
        int glyphs = 0;
        for (QQuickItem *item : menuItems(menu)) {
            if (item == nullptr || !item->isVisible() || item->inherits("QQuickMenuSeparator")
                || iconOf(item).isEmpty()) {
                continue;
            }
            const QString name = withoutMnemonic(item->property("text").toString());
            QQuickItem *icon = childNamed(item, "appMenuItemIcon");
            QQuickItem *label = childNamed(item, "appMenuItemLabel");
            QQuickItem *row = childNamed(item, "appMenuItemRow");
            QVERIFY2(icon != nullptr && label != nullptr && row != nullptr, qPrintable(name));
            QVERIFY2(icon->isVisible(), qPrintable(name));
            QCOMPARE(icon->width(), qreal(glyph));
            QCOMPARE(icon->height(), qreal(glyph));
            const qreal drawnGap = label->x() - icon->mapToItem(row, QPointF(icon->width(), 0)).x();
            QVERIFY2(qFuzzyCompare(drawnGap, qreal(gap)), qPrintable(QStringLiteral("%1: %2").arg(name).arg(drawnGap)));
            // The row holds the glyph.
            QVERIFY2(item->height() >= glyph + item->property("topPadding").toReal()
                                           + item->property("bottomPadding").toReal(),
                     qPrintable(name));
            ++glyphs;
        }
        QVERIFY(glyphs >= 3);
    };

    QObject *menuBar = menuBarOf(window);
    QObject *edit = menuNamed(window, "editMenu");
    QVERIFY(edit != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(menuBarItemAt(menuBar, 1)).center().toPoint());
    QTRY_VERIFY(edit->property("opened").toBool());
    checkDrawn(edit);
    if (QTest::currentTestFailed()) {
        return;
    }
    QMetaObject::invokeMethod(edit, "close");
    QTRY_VERIFY(!edit->property("visible").toBool());

    // A context menu: the library's.
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRootCount", 1);
    main.libraryModel.setExpandedKeys({kArtistKey, kAlbumKey});
    main.libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree());
    QTRY_VERIFY(viewRow(window, "libraryView", 1) != nullptr);
    rightClick(window, viewRow(window, "libraryView", 1));
    QObject *library = nullptr;
    QTRY_VERIFY(QTest::qWaitFor([&]() {
        library = visibleMenu(window, QStringLiteral("libraryContextMenu"));
        return library != nullptr && library->property("opened").toBool();
    }));
    checkDrawn(library);
    QMetaObject::invokeMethod(library, "close");
    QTRY_VERIFY(!library->property("visible").toBool());
}

// Every menu item in the app's sources is an AppMenuItem, or a component
// made from one, so every menu's glyphs follow the font; and its words
// show markup as text, with the mnemonic underlined.
void UiMenusTest::everyMenuItemIsAnAppMenuItem() {
    const QDir qml(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml"));
    QDirIterator files(qml.path(), {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
    const QRegularExpression bareItem(QStringLiteral(R"((^|[^A-Za-z.])(Controls\.|T\.)?MenuItem\s*\{)"),
                                      QRegularExpression::MultilineOption);
    int files_ = 0;
    while (files.hasNext()) {
        const QString path = files.next();
        const QString file = qml.relativeFilePath(path);
        if (file == QStringLiteral("components/AppMenuItem.qml")) {
            continue;
        }
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(source.readAll());
        QVERIFY2(!bareItem.match(text).hasMatch(), qPrintable(file));
        ++files_;
    }
    QVERIFY(files_ > 20);

    // At a large font, where a glyph is wider than Breeze's.
    ScopedApplicationFont font(24);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "components" as Components

ApplicationWindow {
    width: 300
    height: 200
    visible: true
    property alias menu: menu
    Components.AppMenu {
        id: menu
        Components.AppMenuItem { objectName: "markup"; text: "<b>Bold</b> && \"Q\" &Open"; icon.name: "document-open" }
        Components.AppMenu {
            title: "&More"
            Components.AppMenuItem { text: "&Inner" }
        }
    }
}
)QML"),
                      QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/AppMenuItemHarness.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *menu = qvariant_cast<QObject *>(root->property("menu"));
    QMetaObject::invokeMethod(menu, "open");
    QTRY_VERIFY(menu->property("opened").toBool());
    QQuickItem *item = menuItems(menu).value(0);
    QQuickItem *submenuItem = menuItems(menu).value(1);
    QVERIFY(item != nullptr && submenuItem != nullptr);
    QQuickItem *label = childNamed(item, "appMenuItemLabel");
    QVERIFY(label != nullptr);
    // The menu makes a submenu's item, an AppMenuItem as well: its words
    // in the other items' column.
    QQuickItem *submenuLabel = childNamed(submenuItem, "appMenuItemLabel");
    QVERIFY(submenuLabel != nullptr);
    QTRY_COMPARE(submenuLabel->mapToScene(QPointF()).x(), label->mapToScene(QPointF()).x());
    // The words' own markup is escaped, and shows as text.
    QCOMPARE(label->property("text").toString(), QStringLiteral("&lt;b&gt;Bold&lt;/b&gt; &amp; &quot;Q&quot; Open"));
    // Alt held: the mnemonic, underlined, is the only markup.
    QVariant underlined;
    QVERIFY(QMetaObject::invokeMethod(item, "styledLabel", Q_RETURN_ARG(QVariant, underlined),
                                      Q_ARG(QVariant, QStringLiteral("<b>Bold</b> && \"Q\" &Open")),
                                      Q_ARG(QVariant, true)));
    QCOMPARE(underlined.toString(), QStringLiteral("&lt;b&gt;Bold&lt;/b&gt; &amp; &quot;Q&quot; <u>O</u>pen"));
    QMetaObject::invokeMethod(menu, "close");
    QTRY_VERIFY(!menu->property("visible").toBool());
}

void UiMenusTest::marksShareTheGlyphColumn_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// In View's and Playback's menus, where no item has both a check mark and
// a glyph, marks and glyphs share one column, as in Breeze's widget menus:
// as wide as the wider, each centred in it, with every item's words on
// one edge a spacing after it. Each had a column of its own, which left a
// mark a glyph's width and two spacings from its words. The menus without
// marks keep their column where it was.
void UiMenusTest::marksShareTheGlyphColumn() {
    QFETCH(qreal, pointSize);
    ScopedApplicationFont font(pointSize);
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *menuBar = menuBarOf(window);

    // Where an item's words begin, in the item.
    const auto labelX = [](QQuickItem *item) {
        QQuickItem *label = childNamed(item, "appMenuItemLabel");
        return label != nullptr ? label->mapToItem(item, QPointF()).x() : -1.0;
    };
    // The words' left edge in a menu without marks: Edit's.
    QObject *edit = menuNamed(window, "editMenu");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(menuBarItemAt(menuBar, 1)).center().toPoint());
    QTRY_VERIFY(edit->property("opened").toBool());
    QQuickItem *undo = menuItemWithText(edit, QStringLiteral("Undo"));
    QVERIFY(undo != nullptr);
    QQuickItem *undoGlyph = childNamed(undo, "appMenuItemIcon");
    QVERIFY(undoGlyph != nullptr);
    const qreal glyphColumnLeft = undoGlyph->mapToItem(undo, QPointF()).x();
    const qreal glyph = undoGlyph->width();
    const qreal plainWordsX = labelX(undo);
    QMetaObject::invokeMethod(edit, "close");
    QTRY_VERIFY(!edit->property("visible").toBool());

    const QList<std::pair<int, const char *>> menus{{2, "viewMenu"}, {3, "playbackMenu"}};
    for (const auto &[index, name] : menus) {
        QObject *menu = menuNamed(window, name);
        QVERIFY(menu != nullptr);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          sceneRect(menuBarItemAt(menuBar, index)).center().toPoint());
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(!menu->property("hasMarkedGlyphs").toBool());

        qreal wordsX = -1.0;
        qreal columnCentre = -1.0;
        qreal markWidth = 0.0;
        int marks = 0;
        int glyphs = 0;
        QList<QQuickItem *> columns;
        for (QQuickItem *item : menuItems(menu)) {
            if (item == nullptr || !item->isVisible() || item->inherits("QQuickMenuSeparator")) {
                continue;
            }
            const QString text = QStringLiteral("%1: %2").arg(QLatin1String(name), item->property("text").toString());
            // Every item's words, the heading's too, on one edge.
            if (wordsX < 0) {
                wordsX = labelX(item);
            }
            QCOMPARE(labelX(item), wordsX);
            QQuickItem *column = childNamed(item, "appMenuItemGlyphColumn");
            QVERIFY2(column != nullptr && column->isVisible(), qPrintable(text));
            const qreal centre = column->mapToItem(item, QPointF(column->width() / 2, 0)).x();
            if (columnCentre < 0) {
                columnCentre = centre;
            }
            QCOMPARE(centre, columnCentre);
            if (item->property("checkable").toBool()) {
                auto *indicator = qvariant_cast<QQuickItem *>(item->property("indicator"));
                QVERIFY2(indicator != nullptr && indicator->isVisible(), qPrintable(text));
                markWidth = indicator->width();
                // The mark, centred in the column, its words a spacing on.
                QVERIFY2(std::abs(indicator->x() + indicator->width() / 2 - columnCentre) <= 0.5, qPrintable(text));
                const qreal gap = wordsX - (indicator->x() + indicator->width());
                // Closer than a glyph's width: with a column each, a glyph
                // and two spacings.
                QVERIFY2(gap > 0 && gap < glyph, qPrintable(QStringLiteral("%1: %2").arg(text).arg(gap)));
                ++marks;
            }
            if (!iconOf(item).isEmpty()) {
                QQuickItem *icon = childNamed(item, "appMenuItemIcon");
                QVERIFY2(icon != nullptr && icon->isVisible(), qPrintable(text));
                QCOMPARE(icon->width(), glyph);
                const qreal iconCentre = icon->mapToItem(item, QPointF(icon->width() / 2, 0)).x();
                QVERIFY2(std::abs(iconCentre - columnCentre) <= 0.5, qPrintable(text));
                ++glyphs;
            }
            // Where the glyphs of a menu without marks begin.
            QCOMPARE(column->mapToItem(item, QPointF()).x(), glyphColumnLeft);
            columns << column;
        }
        QVERIFY2(marks >= 3 && glyphs >= 1, name);
        // The column: the wider of mark and glyph.
        for (QQuickItem *column : std::as_const(columns)) {
            QCOMPARE(column->width(), std::max(markWidth, glyph));
        }
        QVERIFY(wordsX <= plainWordsX + std::max(0.0, markWidth - glyph));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
    }
}

// A menu with an item that has both a check mark and a glyph gives each a
// column of its own, the mark first, as Breeze's item does.
void UiMenusTest::markedGlyphsKeepTwoColumns() {
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "components" as Components

ApplicationWindow {
    width: 300
    height: 200
    visible: true
    property alias menu: menu
    Components.AppMenu {
        id: menu
        Components.AppMenuItem { objectName: "both"; text: "&Both"; checkable: true; icon.name: "media-playlist-shuffle" }
        Components.AppMenuItem { objectName: "mark"; text: "&Mark"; checkable: true }
        Components.AppMenuItem { objectName: "glyph"; text: "&Glyph"; icon.name: "document-open" }
    }
}
)QML"),
                      QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/MarkedGlyphsHarness.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *menu = qvariant_cast<QObject *>(root->property("menu"));
    QMetaObject::invokeMethod(menu, "open");
    QTRY_VERIFY(menu->property("opened").toBool());
    QTRY_VERIFY(menu->property("hasMarkedGlyphs").toBool());
    const QList<QQuickItem *> items = menuItems(menu);
    QCOMPARE(items.size(), 3);
    qreal wordsX = -1.0;
    for (QQuickItem *item : items) {
        QQuickItem *label = childNamed(item, "appMenuItemLabel");
        QQuickItem *column = childNamed(item, "appMenuItemGlyphColumn");
        QVERIFY(label != nullptr && column != nullptr);
        if (wordsX < 0) {
            wordsX = label->mapToItem(item, QPointF()).x();
        }
        QCOMPARE(label->mapToItem(item, QPointF()).x(), wordsX);
        // The glyph's column, a glyph wide, after the mark's.
        QCOMPARE(column->width(), QQmlProperty::read(item, QStringLiteral("icon.width")).toReal());
        if (item->property("checkable").toBool()) {
            auto *indicator = qvariant_cast<QQuickItem *>(item->property("indicator"));
            QVERIFY(indicator != nullptr);
            QVERIFY2(indicator->x() + indicator->width() <= column->mapToItem(item, QPointF()).x(),
                     qPrintable(item->objectName()));
        }
    }
    QMetaObject::invokeMethod(menu, "close");
    QTRY_VERIFY(!menu->property("visible").toBool());
}

// Show in Library shows the library's tree, not a jump arrow, in the
// queue's menu and the search result's (tst_search_palette), from Breeze
// and Breeze Dark alike.
void UiMenusTest::showInLibraryShowsTheLibraryTree() {
    const QString icon = QStringLiteral("view-list-tree");
    for (const QString &theme : {QStringLiteral("breeze"), QStringLiteral("breeze-dark")}) {
        const QString file = QStringLiteral("icons/%1/actions/16/%2.svg").arg(theme, icon);
        QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation, file).isEmpty(), qPrintable(file));
    }

    const auto iconsNamed = [](const char *file) {
        QFile source(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/") + QLatin1String(file));
        if (!source.open(QIODevice::ReadOnly)) {
            return QStringList();
        }
        const QString text = QString::fromUtf8(source.readAll());
        const QRegularExpression item(
            QStringLiteral("\"Show in Library\"\\)\\s*\\n(?:\\s*//[^\\n]*\\n)*\\s*icon\\.name:\\s*\"([^\"]+)\""));
        QStringList names;
        for (const QRegularExpressionMatch &match : item.globalMatch(text)) {
            names << match.captured(1);
        }
        return names;
    };
    QCOMPARE(iconsNamed("panes/QueuePane.qml"), QStringList{icon});
    QCOMPARE(iconsNamed("dialogs/GlobalSearchDialog.qml"), QStringList{icon});
}

// Clear Queue shows Breeze's broom, a line glyph of the weight of Undo's,
// Redo's and Find's beside it (checkMenusListTheirItemsInOrder), which
// Breeze and Breeze Dark both have at 16 px.
void UiMenusTest::clearQueueShowsALineGlyph() {
    for (const QString &theme : {QStringLiteral("breeze"), QStringLiteral("breeze-dark")}) {
        const QString file = QStringLiteral("icons/%1/actions/16/edit-clear-all.svg").arg(theme);
        QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation, file).isEmpty(), qPrintable(file));
    }
    QFile main(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/Main.qml"));
    QVERIFY(main.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(main.readAll());
    const qsizetype action = text.indexOf(QStringLiteral("id: clearQueueAction"));
    QVERIFY(action >= 0);
    const qsizetype icon = text.indexOf(QStringLiteral("icon.name:"), action);
    QVERIFY(icon >= 0);
    QCOMPARE(text.mid(icon, text.indexOf(QLatin1Char('\n'), icon) - icon),
             QStringLiteral("icon.name: \"edit-clear-all\""));
}

void UiMenusTest::checkMenusListTheirItemsInOrder(QQuickWindow *window) {

    QCOMPARE(rowsOf(menuNamed(window, "fileMenu")),
             (QStringList{
                 QStringLiteral("item|Open Files…|%1|document-open").arg(standardKey(QKeySequence::Open)),
                 QStringLiteral("item|Add Files to Queue…||media-playlist-append"),
                 QStringLiteral("item|Add Folder to Queue…||folder-add"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Add Library Folder…||folder-music-symbolic"),
                 QStringLiteral("item|Play Whole Library||media-playback-start"),
                 QStringLiteral("item|Add Whole Library to Queue||media-playlist-append"),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Quit|%1|application-exit").arg(standardKey(QKeySequence::Quit)),
             }));
    QCOMPARE(rowsOf(menuNamed(window, "editMenu")),
             (QStringList{
                 QStringLiteral("item|Undo|%1|edit-undo").arg(standardKey(QKeySequence::Undo)),
                 QStringLiteral("item|Redo|%1|edit-redo").arg(standardKey(QKeySequence::Redo)),
                 QStringLiteral("sep"),
                 QStringLiteral("item|Find…|%1|edit-find").arg(standardKey(QKeySequence::Find)),
                 QStringLiteral("sep"),
                 // A line glyph, as the others' (edit-clear-list's is a
                 // filled badge); Breeze and Breeze Dark both have it.
                 QStringLiteral("item|Clear Queue||edit-clear-all"),
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
                 // The plot's two, then the queue's.
                 QStringLiteral("sep"),
                 QStringLiteral("item|Reset Zoom|Ctrl+0|zoom-original"),
                 QStringLiteral("item|Follow Playhead|F|"),
                 QStringLiteral("sep"),
                 QStringLiteral("check|Follow Current Track in Queue||"),
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
                 QStringLiteral("sep"),
                 QStringLiteral("heading|Repeat"),
                 QStringLiteral("radio|Off||"),
                 QStringLiteral("radio|One Track||"),
                 QStringLiteral("radio|Whole Queue||"),
                 QStringLiteral("sep"),
                 // No glyphs: Breeze has no pair that names volume and
                 // reads apart at 16 px.
                 QStringLiteral("item|Volume Up|Alt+Up|"),
                 QStringLiteral("item|Volume Down|Alt+Down|"),
                 QStringLiteral("check|Mute|Ctrl+Shift+M|"),
             }));
    QCOMPARE(rowsOf(menuNamed(window, "settingsMenu")),
             (QStringList{
                 // KDE's place for it, with the board's Ctrl+Alt+, (#458).
                 QStringLiteral("item|Configure Keyboard Shortcuts…|Ctrl+Alt+,|configure-shortcuts"),
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

void UiMenusTest::checkWordingIsTitleCaseWithOneCharacterEllipsis(MainWindowFixture &main) {
    QQuickWindow *window = main.window;
    LibraryTreeModel &libraryModel = main.libraryModel;
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

void UiMenusTest::checkPlayPauseItemNamesWhatItDoes(QQuickWindow *window) {
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

void UiMenusTest::checkHeadingsAreTextThatIsNotPicked(QQuickWindow *window) {
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

void UiMenusTest::checkHeadingsAreSkippedByThePointerAndTheKeyboard(QQuickWindow *window) {
    QObject *bridge = bridgeOf(window);
    QObject *menu = menuNamed(window, "playbackMenu");
    auto *heading = menu->findChild<QQuickItem *>(QStringLiteral("repeatHeading"));
    QQuickItem *shuffle = menuItemWithText(menu, QStringLiteral("Shuffle"));
    QQuickItem *off = menuItemWithText(menu, QStringLiteral("Off"));
    QVERIFY(heading != nullptr && shuffle != nullptr && off != nullptr);

    // A queue, for Next to be an item the keys reach.
    bridge->setProperty("queueLength", 3);
    const auto emptyQueue = qScopeGuard([bridge] { bridge->setProperty("queueLength", 0); });
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

void UiMenusTest::checkRepeatRadiosFollowTheBackendAndSetIt(QQuickWindow *window) {
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

void UiMenusTest::checkVisualizationRadiosFollowThePlotSwitch(QQuickWindow *window) {
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

void UiMenusTest::checkScaleItemIsOffInWaveformMode(QQuickWindow *window) {
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

// The level meter as part of the plot: its column runs out to the pane's
// edge, which is the window's and the right edge of the queue's scrollbar
// above the pane, with no strip of the plot's floor beside it; its column
// is far wider than the scrollbar's thumb, and nothing of either overlaps
// the other. Its fill is the plot set's meterFill, the app icon's magenta on
// the spectrogram. It is one meter for both visualization modes, of the
// same input, scale and marks, so one signal reads the same in either:
// only its colours change, its geometry the same in both.
void UiMenusTest::checkLevelMeterIsPartOfThePlot(QQuickWindow *window) {
    QObject *bridge = bridgeOf(window);
    QObject *viewer = child(window, "viewerController");
    bridge->setProperty("showLevelMeter", true);
    // A queue longer than its view, scrolled to its end: the thumb at the
    // bottom of its lane, as near the meter as it comes.
    bridge->setProperty("queueRows", sampleQueueRows(200));
    bridge->setProperty("queueLength", 200);
    auto *meter = qobject_cast<LevelMeterItem *>(child(window, "levelMeter"));
    auto *scrollBar = qobject_cast<QQuickItem *>(child(window, "queueScrollBar"));
    auto *queueView = qobject_cast<QQuickItem *>(child(window, "playlistView"));
    QVERIFY(viewer != nullptr && meter != nullptr && scrollBar != nullptr && queueView != nullptr);
    QTRY_VERIFY(meter->isVisible() && meter->width() > 0);
    QTRY_VERIFY(scrollBar->isVisible() && scrollBar->width() > 0);
    QVERIFY(QMetaObject::invokeMethod(queueView, "positionViewAtEnd"));
    QTRY_VERIFY(scrollBar->property("position").toReal() + scrollBar->property("size").toReal() > 0.999);
    QQuickItem *thumb = nullptr;
    QTRY_VERIFY((thumb = childNamed(scrollBar, "laneScrollBarThumb")) != nullptr
                && thumb->isVisible());
    const auto sceneRect = [](QQuickItem *item) {
        return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    };

    // The pane's edge, the window's, and the scrollbar's lane's.
    const QRectF meterRect = sceneRect(meter);
    const QRectF laneRect = sceneRect(scrollBar);
    const QRectF thumbRect = sceneRect(thumb);
    QCOMPARE(meterRect.right(), double(window->width()));
    QVERIFY2(std::abs(meterRect.right() - laneRect.right()) < 1.0,
             qPrintable(QStringLiteral("meter ends at %1, the lane at %2").arg(meterRect.right()).arg(laneRect.right())));
    QCOMPARE(meter->implicitWidth(), LevelMeterItem::kColumnWidth);
    // The track out to that edge, and on it a 6 px body: a ladder of 3 px
    // rungs on a 4 px pitch rather than a solid bar, which tells it from
    // the scrollbar's solid thumb (2 px rungs on a 3 px pitch read as a
    // hatch).
    QCOMPARE(LevelMeterItem::kTrackLeft + LevelMeterItem::kTrackWidth, meter->width());
    QCOMPARE(LevelMeterItem::kBodyWidth, 6.0);
    QCOMPARE(LevelMeterItem::kRungHeight, 3.0);
    QCOMPARE(LevelMeterItem::kRungPitch, 4.0);
    // Neither overlaps the other: the thumb ends above the pane.
    QVERIFY2(!meterRect.intersects(laneRect) && !meterRect.intersects(thumbRect),
             qPrintable(QStringLiteral("meter %1, %2 lane %3, %4, thumb to %5")
                            .arg(meterRect.top()).arg(meterRect.left())
                            .arg(laneRect.bottom()).arg(laneRect.left()).arg(thumbRect.bottom())));
    QVERIFY(thumbRect.bottom() <= meterRect.top());

    // The spectrogram: the set's fill; the ticks with the scale.
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(palette != nullptr);
    const auto fillOf = [&](const char *set) {
        return qvariant_cast<QObject *>(palette->property(set))->property("meterFill").value<QColor>();
    };
    QCOMPARE(meter->property("fillColor").value<QColor>(), fillOf("spectrogramPlot"));
    QCOMPARE(fillOf("spectrogramPlot"), QColor(QStringLiteral("#c149a0")));
    // Every property of the meter but its colours, which decide
    // what it reads and where on its scale: the same in either mode, and
    // with or without the spectrogram's scale.
    const auto reading = [&]() {
        QVariantMap values;
        for (const char *name : {"sourcePath", "laneCount", "active", "playing", "durationSeconds",
                                 "channelCountHint", "clock", "width", "height"}) {
            values.insert(QString::fromLatin1(name), meter->property(name));
        }
        return values;
    };
    const QVariantMap spectrogramReading = reading();
    bridge->setProperty("showSpectrogramScale", true);
    QTest::qWait(50);
    QCOMPARE(reading(), spectrogramReading);
    // The waveform: its own fill, and the same meter.
    QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1));
    QTRY_COMPARE(meter->property("fillColor").value<QColor>(), fillOf("waveformPlot"));
    QCOMPARE(qobject_cast<LevelMeterItem *>(child(window, "levelMeter")), meter);
    QCOMPARE(reading(), spectrogramReading);
    QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0));
    QTRY_VERIFY(meter->property("fillColor").value<QColor>() == fillOf("spectrogramPlot"));
    // Its marks are where its scale puts -6, -12 and -24 dB, whatever the
    // mode: the meter's own mapping, the same as the levels'.
    const double scaleHeight = 300;
    const std::vector<int> gaps = LevelMeterItem::markGaps(scaleHeight);
    QCOMPARE(gaps.size(), std::size_t(3));
    const double levels[] = {-24.0, -12.0, -6.0};
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        QCOMPARE(gaps[i] + 1, int(std::lround(LevelMeterItem::meterPosition(levels[i], scaleHeight))));
    }
    QCOMPARE(LevelMeterItem::meterPosition(-6.0, scaleHeight), 0.8 * scaleHeight);
}

void UiMenusTest::checkEditMenuNamesItsUndoAndRedoAndClearsTheQueue(QQuickWindow *window) {
    QObject *bridge = bridgeOf(window);
    QObject *editMenu = menuNamed(window, "editMenu");
    QQuickItem *undo = menuItems(editMenu).value(0);
    QQuickItem *redo = menuItems(editMenu).value(1);
    QQuickItem *clear = menuItemWithText(editMenu, QStringLiteral("Clear Queue"));
    QVERIFY(undo != nullptr && redo != nullptr && clear != nullptr);

    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
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

    // Redo is named as Undo is, and only while the backend offers one.
    QCOMPARE(withoutMnemonic(redo->property("text").toString()), QStringLiteral("Redo"));
    QVERIFY(!redo->isEnabled());
    bridge->setProperty("queueRedoId", 8.0);
    const QStringList redoNames{QStringLiteral("Redo Clear Queue"), QStringLiteral("Redo Remove from Queue"),
                                QStringLiteral("Redo Replace Queue"), QStringLiteral("Redo Move in Queue")};
    for (qsizetype kind = 1; kind <= redoNames.size(); ++kind) {
        bridge->setProperty("queueRedoKind", static_cast<int>(kind));
        QTRY_COMPARE(withoutMnemonic(redo->property("text").toString()), redoNames.at(kind - 1));
    }
    QVERIFY(redo->isEnabled());
    QMetaObject::invokeMethod(qvariant_cast<QObject *>(redo->property("action")), "trigger");
    QTRY_VERIFY(bridge->property("queueCommands").toStringList().contains(QStringLiteral("redo 8")));
    bridge->setProperty("queueRedoId", 0.0);
    bridge->setProperty("queueRedoKind", 0);
    QTRY_VERIFY(!redo->isEnabled());

    bridge->setProperty("queueRows", sampleQueueRows(2));
    bridge->setProperty("queueLength", 2);
    QTRY_VERIFY(clear->isEnabled());
    QMetaObject::invokeMethod(qvariant_cast<QObject *>(clear->property("action")), "trigger");
    QTRY_VERIFY(bridge->property("queueCommands").toStringList().contains(QStringLiteral("clear")));
}

void UiMenusTest::checkFindOpensSearchOnCtrlF(QQuickWindow *window) {
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

// The lists of key sequences the settings keep for changed shortcuts
// (#458): QKeySequence::listToString's portable text, read into each
// sequence's portable text and written back, whatever form a sequence comes
// in, without empty sequences or repeats.
void UiMenusTest::standardKeysReadAndWriteTheListsTheSettingsKeep() {
    QCOMPARE(StandardKeys::splitSequences(QStringLiteral("Media Play; Space")),
             (QStringList{QStringLiteral("Media Play"), QStringLiteral("Space")}));
    QCOMPARE(StandardKeys::splitSequences(QStringLiteral("Ctrl+K, Ctrl+P; Shift+F11")),
             (QStringList{QStringLiteral("Ctrl+K, Ctrl+P"), QStringLiteral("Shift+F11")}));
    QVERIFY(StandardKeys::splitSequences(QString()).isEmpty());
    QCOMPARE(StandardKeys::splitSequences(QStringLiteral("Ctrl+P; Ctrl+P")), QStringList{QStringLiteral("Ctrl+P")});

    QCOMPARE(StandardKeys::portableText(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Comma)),
             QStringLiteral("Ctrl+Alt+,"));
    QCOMPARE(StandardKeys::portableText(QStringLiteral("ctrl+shift+m")), QStringLiteral("Ctrl+Shift+M"));
    QVERIFY(StandardKeys::portableText(QString()).isEmpty());
    QVERIFY(StandardKeys::portableText(QKeySequence()).isEmpty());

    QCOMPARE(StandardKeys::joinSequences({QStringLiteral("Media Play"), QVariant::fromValue(QKeySequence(Qt::Key_Space)),
                                          QString(), QStringLiteral("Space")}),
             QStringLiteral("Media Play; Space"));
    QVERIFY(StandardKeys::joinSequences({}).isEmpty());
    QVERIFY(StandardKeys::joinSequences({QString()}).isEmpty());
    // Back and forth.
    const QString text = QStringLiteral("Ctrl+Alt+P; Media Play; Ctrl+=");
    QCOMPARE(StandardKeys::joinSequences(QVariant(StandardKeys::splitSequences(text)).toList()), text);
}

// Each binding of an action's standard key triggers it, not only the first
// (#581): Open's Ctrl+O and Open key, Undo's Ctrl+Z, Alt+Backspace and Undo
// key, Redo's, and Find's Ctrl+F and Find key. The first is the action's
// shortcut, which its menu item shows.
void UiMenusTest::checkStandardKeysTriggerTheirActionsByEveryBinding(QQuickWindow *window) {
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueUndoId", 3.0);
    bridge->setProperty("queueUndoKind", 2);
    bridge->setProperty("queueRedoId", 4.0);
    bridge->setProperty("queueRedoKind", 2);
    QObject *search = child(window, "globalSearchController");
    QObject *fileDialog = child(window, "externalFileDialog");
    QVERIFY(search != nullptr && fileDialog != nullptr);

    struct Key {
        const char *action;
        QKeySequence::StandardKey key;
        // Closes what the action opened.
        std::function<void()> close;
    };
    const QList<Key> keys{
        {"openFilesAction", QKeySequence::Open,
         [&] {
             QTRY_VERIFY(fileDialog->property("visible").toBool());
             QMetaObject::invokeMethod(fileDialog, "close");
             QTRY_VERIFY(!fileDialog->property("visible").toBool());
         }},
        {"undoQueueChangeAction", QKeySequence::Undo, [] {}},
        {"redoQueueChangeAction", QKeySequence::Redo, [] {}},
        {"findAction", QKeySequence::Find,
         [&] {
             QTRY_VERIFY(search->property("dialogVisible").toBool());
             QTest::keyClick(window, Qt::Key_Escape);
             QTRY_VERIFY(!search->property("dialogVisible").toBool());
         }},
    };
    for (const Key &key : keys) {
        QObject *action = child(window, key.action);
        QVERIFY2(action != nullptr, key.action);
        const QList<QKeySequence> bindings = QKeySequence::keyBindings(key.key);
        // Each has more than one wherever the tests run.
        QVERIFY2(bindings.size() > 1, key.action);
        for (const QKeySequence &binding : bindings) {
            const QString name = QStringLiteral("%1 %2").arg(QLatin1String(key.action), binding.toString());
            QVERIFY2(action->property("enabled").toBool(), qPrintable(name));
            QSignalSpy triggered(action, SIGNAL(triggered(QObject *)));
            window->requestActivate();
            QTRY_VERIFY(window->isActive());
            window->contentItem()->forceActiveFocus();
            QTest::keySequence(window, binding);
            QTRY_VERIFY2_WITH_TIMEOUT(triggered.count() == 1, qPrintable(name), 1000);
            key.close();
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        QCOMPARE(action->property("shortcut").toString(), bindings.first().toString());
    }
    // QTest's, as it gives the Open, Undo and Find keys no text.
    dropCapturedMessagesMatching(QRegularExpression(QStringLiteral("^keyToAscii Unhandled key code Qt::Key_\\w+$")));
}

void UiMenusTest::checkShortcutsAreTakenOnceEach(QQuickWindow *window) {

    QHash<QString, QString> owners;
    for (QObject *action : actionsOf(window)) {
        const QVariant shortcut = action->property("shortcut");
        const QVariant keyBindings = action->property("keyBindings");
        QList<QKeySequence> sequences;
        if (keyBindings.isValid()) {
            // A ShortcutAction's: all its bindings.
            for (const QString &binding : keyBindings.toStringList()) {
                sequences << QKeySequence(binding);
            }
        } else if (shortcut.typeId() == QMetaType::Int) {
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
    // Redo's keys are Redo's.
    for (const QKeySequence &redo : QKeySequence::keyBindings(QKeySequence::Redo)) {
        QCOMPARE(owners.value(redo.toString()), QStringLiteral("Redo"));
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

void UiMenusTest::checkShortcutsTriggerTheirActions(QQuickWindow *window) {
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    bridge->setProperty("queueUndoId", 3.0);
    bridge->setProperty("queueUndoKind", 2);
    bridge->setProperty("queueRedoId", 4.0);
    bridge->setProperty("queueRedoKind", 2);

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
        {"redoQueueChangeAction", Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier},
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

void UiMenusTest::checkQueueContextMenuShowsKeysItDoesNotTake(QQuickWindow *window) {
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
                 QStringLiteral("item|Show in Library||view-list-tree"),
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

// The cover's menus, on the now playing cover and in the album art viewer,
// give Replace from iTunes an icon, as the other menus give their items, so
// its text starts where theirs does.
void UiMenusTest::checkCoverMenusGiveReplaceFromItunesAnIcon(QQuickWindow *window) {
    QStringList found;
    for (const QString &name : {QStringLiteral("albumArtMenu"), QStringLiteral("albumArtViewerContextMenu")}) {
        const QList<QObject *> menus = window->findChildren<QObject *>(name);
        for (QObject *menu : menus) {
            const QStringList rows = rowsOf(menu);
            QVERIFY2(!rows.isEmpty(), qPrintable(name));
            QCOMPARE(rows.first(), QStringLiteral("item|Replace from iTunes\u2026||download"));
        }
        if (!menus.isEmpty()) {
            found << name;
        }
    }
    QVERIFY2(found.contains(QStringLiteral("albumArtMenu")), qPrintable(found.join(QLatin1Char(' '))));
}

void UiMenusTest::checkLibraryContextMenuNamesWhatItActsOn(MainWindowFixture &main) {
    QQuickWindow *window = main.window;
    LibraryTreeModel &libraryModel = main.libraryModel;
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
                 QStringLiteral("item|Open Folder||folder-open"),
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

void UiMenusTest::checkMenusHaveNoTransitionsOfTheirOwn(QQuickWindow *window) {

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

void UiMenusTest::checkSettingsAndHelpOpenTheirPages(QQuickWindow *window) {

    QObject *preferences = child(window, "preferencesDialog");
    QVERIFY(preferences != nullptr);
    QMetaObject::invokeMethod(child(window, "configureAction"), "trigger");
    QTRY_VERIFY(preferences->property("opened").toBool());
    QMetaObject::invokeMethod(preferences, "close");
    QTRY_VERIFY(!preferences->property("visible").toBool());

    // Configure Keyboard Shortcuts, by its Ctrl+Alt+, too (#458); not while
    // a viewer is open, as Configure.
    QObject *shortcuts = child(window, "keyboardShortcutsDialog");
    QObject *shortcutsAction = child(window, "configureShortcutsAction");
    QVERIFY(shortcuts != nullptr && shortcutsAction != nullptr);
    QVERIFY(shortcutsAction->property("enabled").toBool());
    window->requestActivate();
    QTRY_VERIFY(window->isActive());
    QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier | Qt::AltModifier);
    QTRY_VERIFY(shortcuts->property("opened").toBool());
    QMetaObject::invokeMethod(shortcuts, "close");
    QTRY_VERIFY(!shortcuts->property("visible").toBool());
    QTRY_VERIFY(window->isActive());
    QObject *viewer = child(window, "viewerController");
    QVERIFY(viewer != nullptr);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(window->property("viewerOpen").toBool());
    QVERIFY(!shortcutsAction->property("enabled").toBool());
    QVERIFY(!child(window, "configureAction")->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!window->property("viewerOpen").toBool());

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
    QVERIFY(text->property("text").toString().startsWith(QStringLiteral("<b>KDE</b> is a world-wide community")));
    QTest::keyClick(aboutKde->property("contentItem").value<QQuickItem *>()->window(), Qt::Key_Escape);
    QTRY_VERIFY(!aboutKde->property("visible").toBool());

    // Report Bug… opens the project's issue form in the browser; the test
    // only reads where it goes.
    QCOMPARE(window->property("bugReportUrl").toString(),
             QStringLiteral("https://github.com/Tiaxi/ferrous/issues/new"));
}

void UiMenusTest::checkAboutKdeLinksTakeTheKeyboard(QQuickWindow *window) {
    UrlRecorder opened;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &opened, "open");
    const auto unsetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("https")); });

    QObject *dialog = child(window, "aboutKdeDialog");
    QVERIFY(dialog != nullptr);
    QMetaObject::invokeMethod(child(window, "aboutKdeAction"), "trigger");
    QTRY_VERIFY(dialog->property("opened").toBool());
    const QStringList urls{QStringLiteral("https://www.gnu.org/philosophy/free-sw.html"),
                           QStringLiteral("https://community.kde.org/Get_Involved"),
                           QStringLiteral("https://kde.org/donate/"), QStringLiteral("https://kde.org/")};
    QList<QQuickItem *> links;
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(palette != nullptr);
    for (int i = 0; i < 4; ++i) {
        auto *link = dialog->findChild<QQuickItem *>(QStringLiteral("aboutKdeLink%1").arg(i));
        QVERIFY(link != nullptr);
        QCOMPARE(link->property("url").toString(), urls.at(i));
        // The window's link colour, which reads on the dialog (the theme's
        // own link was 4.3:1 on Breeze Light's view).
        QCOMPARE(link->property("color").value<QColor>().name(QColor::HexArgb),
                 palette->property("link").value<QColor>().name(QColor::HexArgb));
        // A link to assistive technology, named as it reads.
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(link);
        QVERIFY(iface != nullptr);
        QCOMPARE(iface->role(), QAccessible::Link);
        QVERIFY(!link->property("text").toString().isEmpty());
        QCOMPARE(iface->text(QAccessible::Name), link->property("text").toString());
        links.push_back(link);
    }
    // In reading order: each link after the one before it.
    for (int i = 1; i < 4; ++i) {
        const QRectF before = sceneRect(links.at(i - 1));
        const QRectF after = sceneRect(links.at(i));
        QVERIFY(after.top() > before.top() || (after.top() == before.top() && after.left() > before.left()));
    }

    // Tab reaches every link, in that order, with the focus ring on it.
    QQuickWindow *dialogWindow = links.first()->window();
    QStringList reached;
    for (int press = 0; press < 12 && reached.size() < 4; ++press) {
        QTest::keyClick(dialogWindow, Qt::Key_Tab);
        QQuickItem *focus = dialogWindow->activeFocusItem();
        if (!links.contains(focus)) {
            continue;
        }
        reached << focus->objectName();
        for (QQuickItem *link : std::as_const(links)) {
            QQuickItem *ring = itemsNamed(link, QStringLiteral("dialogLinkFocusRing")).value(0);
            QVERIFY(ring != nullptr);
            QCOMPARE(ring->isVisible(), link == focus);
        }
    }
    QCOMPARE(reached, (QStringList{QStringLiteral("aboutKdeLink0"), QStringLiteral("aboutKdeLink1"),
                                   QStringLiteral("aboutKdeLink2"), QStringLiteral("aboutKdeLink3")}));

    // Enter and Space open the focused link.
    links.at(2)->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(links.at(2)->hasActiveFocus());
    QTest::keyClick(dialogWindow, Qt::Key_Return);
    QTRY_COMPARE(opened.urls, (QList<QUrl>{QUrl(urls.at(2))}));
    links.at(0)->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(links.at(0)->hasActiveFocus());
    QTest::keyClick(dialogWindow, Qt::Key_Space);
    QTRY_COMPARE(opened.urls, (QList<QUrl>{QUrl(urls.at(2)), QUrl(urls.at(0))}));
    QVERIFY(dialog->property("visible").toBool());
    QMetaObject::invokeMethod(dialog, "close");
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

// A library folder's, an artist's and an album's Open Folder opens that
// folder; a track's Open Containing Folder the one it is in (all four said
// Open Containing Folder, and three opened the row's own).
void UiMenusTest::openFolderNamesWhereItGoes() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    bridgeOf(window)->setProperty("libraryRootCount", 2);
    main.libraryModel.setExpandedKeys({kRootKey, kArtistKey, kAlbumKey});
    main.libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree(true));
    QTRY_VERIFY(viewRow(window, "libraryView", 3) != nullptr);
    const QStringList names{QStringLiteral("Open Folder"), QStringLiteral("Open Folder"),
                            QStringLiteral("Open Folder"), QStringLiteral("Open Containing Folder")};
    for (int row = 0; row < names.size(); ++row) {
        rightClick(window, viewRow(window, "libraryView", row));
        QObject *menu = nullptr;
        QTRY_VERIFY((menu = visibleMenu(window, QStringLiteral("libraryContextMenu"))) != nullptr
                    && menu->property("opened").toBool());
        QString shown;
        for (QQuickItem *item : menuItems(menu)) {
            if (item != nullptr && item->objectName() == QStringLiteral("libraryOpenFolderItem")) {
                shown = withoutMnemonic(item->property("text").toString());
                QCOMPARE(iconOf(item), QStringLiteral("folder-open"));
                QVERIFY(item->isEnabled());
            }
        }
        QCOMPARE(shown, names.at(row));
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
    }
}

// Playback's Play/Pause is off with nothing to play, Stop while stopped,
// Previous and Next while the queue is empty, also as that changes with the
// menu open (all of them read as able to act). The transport's buttons are
// off with the items (on first run they stayed lit beside the menu's
// disabled items). The actions stay on, and with them the shortcuts, which
// act as before.
void UiMenusTest::playbackItemsAreOffWhereTheyCannotAct() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *menu = menuNamed(window, "playbackMenu");
    QVERIFY(menu != nullptr);
    struct Command {
        const char *action;
        Qt::Key key;
        Qt::KeyboardModifiers modifiers;
        QString text;
        const char *button;
    };
    const QList<Command> commands{
        {"playPauseAction", Qt::Key_Space, Qt::NoModifier, QString(), "transportButton_playPause"},
        {"stopAction", Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("Stop"),
         "transportButton_stop"},
        {"previousAction", Qt::Key_Left, Qt::ControlModifier, QStringLiteral("Previous"), "transportButton_previous"},
        {"nextAction", Qt::Key_Right, Qt::ControlModifier, QStringLiteral("Next"), "transportButton_next"},
    };
    const struct {
        QString state;
        int queue;
        bool playPause;
        bool stop;
        bool skip;
    } states[] = {
        {QStringLiteral("Stopped"), 0, false, false, false}, {QStringLiteral("Stopped"), 3, true, false, true},
        {QStringLiteral("Playing"), 3, true, true, true},    {QStringLiteral("Paused"), 3, true, true, true},
        {QStringLiteral("Playing"), 0, true, true, false},   {QStringLiteral("Stopped"), 0, false, false, false},
    };
    // Play/Pause's item names what it does now.
    const auto textOf = [](const Command &command, const QString &state) {
        if (!command.text.isEmpty()) {
            return command.text;
        }
        return state == QStringLiteral("Playing") ? QStringLiteral("Pause") : QStringLiteral("Play");
    };
    for (const bool open : {false, true}) {
        if (open) {
            QMetaObject::invokeMethod(menu, "open");
            QTRY_VERIFY(menu->property("opened").toBool());
        }
        for (const auto &state : states) {
            bridge->setProperty("queueLength", state.queue);
            bridge->setProperty("playbackState", state.state);
            const QByteArray at = QStringLiteral("%1, %2 queued, menu %3")
                                      .arg(state.state).arg(state.queue).arg(open).toUtf8();
            for (const Command &command : commands) {
                QQuickItem *item = menuItemWithText(menu, textOf(command, state.state));
                QVERIFY2(item != nullptr, command.action);
                const bool can = command.button == QByteArrayLiteral("transportButton_playPause") ? state.playPause
                    : command.text == QStringLiteral("Stop")                                    ? state.stop
                                                                                                : state.skip;
                // The item's own switch: a closed menu's items are in no
                // window to be enabled in.
                QVERIFY2(item->property("enabled").toBool() == can, (command.action + QByteArray(": ") + at).constData());
                QObject *action = child(window, command.action);
                QVERIFY2(action->property("enabled").toBool(), at.constData());
                // The transport's button of the same action, off with it.
                QQuickItem *button = named(window, command.button);
                QVERIFY2(button != nullptr, command.button);
                QVERIFY2(button->isEnabled() == can, (command.button + QByteArray(": ") + at).constData());
            }
        }
    }

    // With nothing to act on (the last state), a click on an item that is
    // off does nothing.
    QQuickItem *next = menuItemWithText(menu, QStringLiteral("Next"));
    QVERIFY(next->window() != nullptr);
    QSignalSpy picked(child(window, "nextAction"), SIGNAL(triggered(QObject *)));
    QTest::mouseClick(next->window(), Qt::LeftButton, Qt::NoModifier, sceneRect(next).center().toPoint());
    QCoreApplication::processEvents();
    QCOMPARE(picked.count(), 0);
    QMetaObject::invokeMethod(menu, "close");
    QTRY_VERIFY(!menu->property("visible").toBool());
    window->requestActivate();
    QTRY_VERIFY(window->isActive());
    window->contentItem()->forceActiveFocus();

    // The keys trigger their actions whether the items and the buttons are
    // off or on: what they then do is the backend's, as it is MPRIS's, which
    // calls the bridge and no action. A button acts only while it is on.
    for (const int queued : {0, 3}) {
        bridge->setProperty("queueLength", queued);
        bridge->setProperty("playbackState", queued > 0 ? QStringLiteral("Playing") : QStringLiteral("Stopped"));
        for (const Command &command : commands) {
            QObject *action = child(window, command.action);
            QSignalSpy triggered(action, SIGNAL(triggered(QObject *)));
            QTest::keyClick(window, command.key, command.modifiers);
            QTRY_COMPARE(triggered.count(), 1);
            QQuickItem *button = named(window, command.button);
            QVERIFY(button != nullptr);
            // Play/Pause's button acts by Play or Pause, as it shows them.
            auto *buttonAction = qvariant_cast<QObject *>(button->property("action"));
            QVERIFY(buttonAction != nullptr);
            QSignalSpy clicked(buttonAction, SIGNAL(triggered(QObject *)));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(button).center().toPoint());
            QCoreApplication::processEvents();
            QTRY_COMPARE(clicked.count(), queued > 0 ? 1 : 0);
        }
    }
}

// The button copies its action's enabled whenever that changes: an item
// that cannot act stays off as its action comes back, and one that can is
// off with its action.
void UiMenusTest::playbackItemsKeepTheirStateAsTheActionsChange() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *menu = menuNamed(window, "playbackMenu");
    QQuickItem *next = menuItemWithText(menu, QStringLiteral("Next"));
    QObject *action = child(window, "nextAction");
    QVERIFY(next != nullptr && action != nullptr);
    for (const int queued : {0, 3, 0}) {
        bridge->setProperty("queueLength", queued);
        for (const bool actionOn : {false, true}) {
            QVERIFY(action->setProperty("enabled", actionOn));
            QCOMPARE(next->property("enabled").toBool(), actionOn && queued > 0);
        }
    }
}

// A heading inside a menu starts a group, with a separator over it unless
// it is first (Shuffle ran into Repeat's).
void UiMenusTest::everyMenuHeadingStartsItsGroup() {
    MainWindowFixture main(wideWindow());
    QQuickWindow *window = main.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    int headings = 0;
    for (QObject *menu : menusOf(window)) {
        const QStringList rows = rowsOf(menu);
        for (qsizetype i = 0; i < rows.size(); ++i) {
            if (!rows.at(i).startsWith(QStringLiteral("heading|"))) {
                continue;
            }
            QVERIFY2(i == 0 || rows.at(i - 1) == QStringLiteral("sep"), qPrintable(rows.at(i)));
            ++headings;
        }
    }
    QCOMPARE(headings, 2);
}


int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h.
    isolateFromDesktopSessionWithAppLook();
    // Settings, data and caches of the test's own.
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    QApplication app(argc, argv);
    returnFocusFromClosedPopups();
    UiMenusTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_menus.moc"
