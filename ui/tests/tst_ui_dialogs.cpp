// SPDX-License-Identifier: GPL-3.0-or-later

// The dialogs of #373: Configure Ferrous, About, Edit Tags, Recent Issues
// and the smaller dialogs, from the QML sources with the fallback bridge.

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>
#include <QtTest/QtTest>
#include <qqml.h>
#include <algorithm>
#include <functional>
#include <limits>

#include "../src/AboutComponents.h"
#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/QueueRows.h"
#include "../src/Localization.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"
#include "../src/WindowPresence.h"
// The tests answer the tag editor's file operations themselves.
#define private public
#include "../src/TagEditorController.h"
#undef private
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

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
QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel,
                             int width = 1600, int height = 1000) {
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("0.3.0"));
    engine.rootContext()->setContextProperty(QStringLiteral("appComponents"), aboutComponents());
    engine.load(QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    window->resize(width, height);
    window->requestActivate();
    if (!QTest::qWaitForWindowActive(window)
        || !QTest::qWaitFor([&]() { return window->width() == width && window->height() == height; })) {
        return nullptr;
    }
    return window;
}

QObject *bridgeOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiBridge"));
}

QObject *paletteOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiPalette"));
}

QColor tokenOf(QQuickWindow *window, const char *name) {
    return paletteOf(window)->property(name).value<QColor>();
}

QString hex(const QColor &color) {
    return color.name(QColor::HexArgb);
}

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

// The items of the window, popups included, as the overlay holds them.
QQuickItem *named(QQuickWindow *window, const QString &name) {
    const QList<QQuickItem *> found = itemsNamed(window->contentItem(), name);
    return found.isEmpty() ? nullptr : found.first();
}

QQuickItem *visibleNamed(QQuickWindow *window, const QString &name) {
    for (QQuickItem *item : itemsNamed(window->contentItem(), name)) {
        if (item->isVisible()) {
            return item;
        }
    }
    return nullptr;
}

QObject *object(QQuickWindow *window, const QString &name) {
    return window->findChild<QObject *>(name);
}

QVariant evaluate(QObject *scope, const QString &expression) {
    QQmlExpression expr(qmlContext(scope), scope, expression);
    return expr.evaluate();
}

QRectF sceneRect(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void click(QQuickWindow *window, QQuickItem *item) {
    const QPointF centre = sceneRect(item).center();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
}

// The item a press at `scenePos` goes to: of the shown items there that
// take the left button, the one painted last, siblings in their z order.
QQuickItem *pressTargetAt(QQuickWindow *window, const QPointF &scenePos) {
    QQuickItem *target = nullptr;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible()) {
            return;
        }
        if ((item->acceptedMouseButtons() & Qt::LeftButton) && item->contains(item->mapFromScene(scenePos))) {
            target = item;
        }
        QList<QQuickItem *> children = item->childItems();
        std::stable_sort(children.begin(), children.end(),
                         [](QQuickItem *a, QQuickItem *b) { return a->z() < b->z(); });
        for (QQuickItem *child : children) {
            visit(child);
        }
    };
    visit(window->contentItem());
    return target;
}

QString describe(QQuickItem *item) {
    if (item == nullptr) {
        return QStringLiteral("nothing");
    }
    return QStringLiteral("%1 \"%2\"").arg(QString::fromLatin1(item->metaObject()->className()),
                                         item->objectName());
}

// Whether `item` or an item it sits in still waits for its polish, where
// layouts and positioners place their children. A Row culls a child that is
// hidden and takes it back only there: until then, a child shown again says
// it is visible, but Qt neither draws it nor gives it presses.
bool layoutPending(const QQuickItem *item) {
    for (; item != nullptr; item = item->parentItem()) {
        if (QQuickTest::qIsPolishScheduled(item)) {
            return true;
        }
    }
    return false;
}

// Clicks `button` once it is enabled, laid out and nothing covers it, and
// checks that the click reached it: a failure names what took the press
// instead.
bool clickButtonWhenReachable(QQuickWindow *window, QQuickItem *button, QString *failure) {
    const auto reachable = [&]() {
        return button->isVisible() && button->isEnabled() && !layoutPending(button)
            && pressTargetAt(window, sceneRect(button).center()) == button;
    };
    if (!QTest::qWaitFor(reachable)) {
        *failure = QStringLiteral("%1 is %2, %3, %4; a press there goes to %5")
                       .arg(describe(button),
                            button->isVisible() ? QStringLiteral("shown") : QStringLiteral("hidden"),
                            button->isEnabled() ? QStringLiteral("enabled") : QStringLiteral("disabled"),
                            layoutPending(button) ? QStringLiteral("not laid out") : QStringLiteral("laid out"),
                            describe(pressTargetAt(window, sceneRect(button).center())));
        return false;
    }
    QSignalSpy clicked(button, SIGNAL(clicked()));
    click(window, button);
    if (clicked.count() != 1) {
        *failure = QStringLiteral("%1 had %2 clicks").arg(describe(button)).arg(clicked.count());
        return false;
    }
    return true;
}

bool emitClicked(QObject *button) {
    return button != nullptr && QMetaObject::invokeMethod(button, "clicked");
}

// Presses Tab until `target` has focus, at most `limit` times.
bool tabTo(QQuickWindow *window, QQuickItem *target, int limit = 60,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    for (int presses = 0; presses < limit && !target->hasActiveFocus(); ++presses) {
        QTest::keyClick(window, Qt::Key_Tab, modifiers);
    }
    return target->hasActiveFocus();
}

// The preferences dialog, opened on `page`.
QObject *openPreferences(QQuickWindow *window, int page = 0) {
    QObject *dialog = object(window, QStringLiteral("preferencesDialog"));
    if (dialog == nullptr || !QMetaObject::invokeMethod(dialog, "openPage", Q_ARG(QVariant, page))) {
        return nullptr;
    }
    if (!QTest::qWaitFor([&]() { return dialog->property("opened").toBool(); })) {
        return nullptr;
    }
    return dialog;
}

QObject *settingsOf(QObject *dialog) {
    return qvariant_cast<QObject *>(dialog->property("settings"));
}

QVariant pendingValue(QObject *dialog, const QString &key) {
    QVariant value;
    QMetaObject::invokeMethod(settingsOf(dialog), "value", Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, key));
    return value;
}

// Tag rows as the tag editor's load answers them: `count` tracks of one
// album, all in the genre Classical.
QByteArray tagRows(int count) {
    QJsonArray rows;
    QJsonArray paths;
    for (int i = 0; i < count; ++i) {
        const QString path = QStringLiteral("/fixture/Album/%1 Song.flac").arg(i + 1, 2, 10, QLatin1Char('0'));
        paths.push_back(path);
        rows.push_back(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("fileName"), QStringLiteral("%1 Song.flac").arg(i + 1, 2, 10, QLatin1Char('0'))},
            {QStringLiteral("directory"), QStringLiteral("/fixture/Album")},
            {QStringLiteral("formatKind"), QStringLiteral("FLAC")},
            {QStringLiteral("title"), QStringLiteral("Song %1").arg(i + 1)},
            {QStringLiteral("artist"), QStringLiteral("Artist")},
            {QStringLiteral("album"), QStringLiteral("Album")},
            {QStringLiteral("genre"), QStringLiteral("Classical")},
            {QStringLiteral("trackNo"), QString::number(i + 1)},
        });
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("resolvedPaths"), paths},
                                     {QStringLiteral("rows"), rows}})
        .toJson(QJsonDocument::Compact);
}

// The window's issue history, with an entry reported.
void report(QQuickWindow *window, int category, int severity, const QString &subject, const QString &message) {
    QObject *errors = object(window, QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, category), Q_ARG(QVariant, severity),
                                      Q_ARG(QVariant, subject), Q_ARG(QVariant, message)));
}

// Opens Recent Issues and waits until its rows are laid out.
bool openIssues(QQuickWindow *window) {
    QObject *popup = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    if (popup == nullptr || !QMetaObject::invokeMethod(popup, "open")) {
        return false;
    }
    return QTest::qWaitFor([&]() {
        QQuickItem *view = visibleNamed(window, QStringLiteral("errorHistoryView"));
        return popup->property("opened").toBool() && view != nullptr && view->height() > 0
            && view->height() >= qvariant_cast<QQuickItem *>(view->property("contentItem"))->property("contentHeight").toReal() - 0.5;
    });
}

// Queue rows of `paths`, as the fallback bridge takes them.
QVariantList queueRowsOf(const QStringList &paths) {
    QVariantList rows;
    for (const QString &path : paths) {
        rows.push_back(QVariantMap{{QStringLiteral("title"), QFileInfo(path).completeBaseName()},
                                   {QStringLiteral("path"), path},
                                   {QStringLiteral("lengthText"), QStringLiteral("1:00")}});
    }
    return rows;
}

const QString kStaticPath = QStringLiteral("/music/The Quiet Meridian/Small Hours/04 - Static.flac");
const QString kEndsEarly = QStringLiteral(
    "04 - Static.flac ends early. The last FLAC frame is cut off, so playback stops at 4:02.");

// Every object under `root` whose `text` or `title` property holds `needle`.
QStringList textsHolding(QObject *root, const QString &needle) {
    QStringList found;
    const std::function<void(QObject *)> visit = [&](QObject *object) {
        for (const char *property : {"text", "title"}) {
            const QVariant value = object->property(property);
            if (value.typeId() == QMetaType::QString && value.toString().contains(needle)) {
                found.push_back(QStringLiteral("%1 (%2): %3")
                                    .arg(QString::fromLatin1(object->metaObject()->className()),
                                         object->objectName(), value.toString()));
            }
        }
        for (QObject *child : object->children()) {
            visit(child);
        }
    };
    visit(root);
    return found;
}

} // namespace

class UiDialogsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void configureIsAPageListWithKdesCaptionAndButtons();
    void configurePagesMoveByKeyboardAndTabReachesEveryControl();
    void configureAppliesOnOkOrApplyAndCancelDropsTheChanges();
    void configureDefaultsSetsThePagesBackToBeApplied();
    void enterPressesConfiguresOkButtonUnlessAControlTakesIt();
    void enterInATextFieldTakesTheTextNotTheDefaultButton();
    void everyConfigureSettingReachesTheBridge();
    void titleStripSettingIsOnlyForTheWholeScreen();
    void configureHelpTextIsSecondaryAndSettingsKeepTheirSliderRange();
    void libraryFoldersActOnTheSelectedFolderAtOnce();
    void lastFmPageShowsOnlyTheButtonsThatApply();
    void aboutShowsTheIconVersionTabsAndCopiesTheVersion();
    void recentIssuesIsAPopupAboveTheStatusBar();
    void recentIssuesRowsSplitTheMessageAndOfferOneAction();
    void recentIssuesActionsShowTheFileOrOpenTheSettings();
    void recentIssuesClearAllDiagnosticsAndKeyboard();
    void recentIssuesAreSeenWhenThePopupCloses();
    void thePanesEndsEarlyLinkOpensItsIssue();
    void recentIssuesFollowAQueueReplacedWithOneOfTheSameLength();
    void theQueuesPathIndexFollowsEveryEdit();
    void recentIssuesLookUpALargeQueueAtTheSameCost();
    void editTagsMarksAChangedFieldAndColumnAndReverts();
    void editTagsColumnRevertIsClickedOnceTheHeaderIsLaidOut();
    void editTagsTableHeaderUsesTheQueuesHeaderStyle();
    void editTagsToolsMenuHoldsTheToolsAndCaseChanges();
    void dialogsPutTheirButtonsInKdesOrder();
    void dialogsUseOneCharacterEllipsesAndNoTextGlyphs();
};

void UiDialogsTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(captureMessage);
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
}

void UiDialogsTest::init() {
    takeMessages();
}

void UiDialogsTest::cleanup() {
    const QString messages = takeMessages();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiDialogsTest::cleanupTestCase() {
    qInstallMessageHandler(previousHandler());
    previousHandler() = nullptr;
}

void UiDialogsTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiDialogsTest::configureIsAPageListWithKdesCaptionAndButtons() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = openPreferences(window);
    QVERIFY(dialog != nullptr);
    // KConfigDialog's caption.
    QCOMPARE(dialog->property("title").toString(), QStringLiteral("Configure \u2014 Ferrous"));

    // A 176 px list of the five pages on the view colour, the selected one
    // a full-row tint.
    QQuickItem *list = visibleNamed(window, QStringLiteral("preferencesPageList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->width(), 176.0);
    QCOMPARE(hex(list->parentItem()->property("color").value<QColor>()), hex(tokenOf(window, "view")));
    const QStringList pages{QStringLiteral("Library"), QStringLiteral("Spectrogram"), QStringLiteral("Appearance"),
                            QStringLiteral("Last.fm"), QStringLiteral("Media Controls")};
    QQuickItem *stack = nullptr;
    QTRY_VERIFY((stack = visibleNamed(window, QStringLiteral("preferencesPages"))) != nullptr);
    for (int i = 0; i < pages.size(); ++i) {
        QQuickItem *row = visibleNamed(window, QStringLiteral("preferencesPage_%1").arg(i));
        QVERIFY(row != nullptr);
        QCOMPARE(row->property("text").toString(), pages[i]);
        QCOMPARE(row->width(), 176.0);
        click(window, row);
        QTRY_COMPARE(stack->property("currentIndex").toInt(), i);
        // The page's title, 15 px semibold, on the content edge.
        QQuickItem *page = stack->childItems().at(i);
        QQuickItem *title = itemsNamed(page, QStringLiteral("preferencesPageTitle")).value(0);
        QVERIFY(title != nullptr);
        QCOMPARE(title->property("text").toString(), pages[i]);
        const QFont font = title->property("font").value<QFont>();
        QCOMPARE(font.weight(), QFont::DemiBold);
        // 15 px with the default 10 pt font.
        QCOMPARE(font.pointSizeF(), paletteOf(window)->property("basePointSize").toReal() * 1.125);
        QCOMPARE(sceneRect(title).left(), sceneRect(page).left() + 24.0);
        QQuickItem *tint = itemsNamed(row, QStringLiteral("rowBackgroundTint")).value(0);
        QVERIFY(tint != nullptr && tint->isVisible());
        QCOMPARE(tint->width(), row->width());
    }

    // Defaults on the left; OK, Apply and Cancel on the right, in that order.
    QQuickItem *defaults = visibleNamed(window, QStringLiteral("preferencesDefaultsButton"));
    QQuickItem *ok = visibleNamed(window, QStringLiteral("preferencesOkButton"));
    QQuickItem *apply = visibleNamed(window, QStringLiteral("preferencesApplyButton"));
    QQuickItem *cancel = visibleNamed(window, QStringLiteral("preferencesCancelButton"));
    QVERIFY(defaults != nullptr && ok != nullptr && apply != nullptr && cancel != nullptr);
    QVERIFY(sceneRect(defaults).right() < sceneRect(ok).left());
    QVERIFY(sceneRect(ok).right() < sceneRect(apply).left());
    QVERIFY(sceneRect(apply).right() < sceneRect(cancel).left());
    const QRectF footer = sceneRect(qvariant_cast<QQuickItem *>(dialog->property("footer")));
    QVERIFY(sceneRect(defaults).right() < footer.center().x());
    QVERIFY(sceneRect(ok).left() > footer.center().x());
    // OK is the default button.
    QVERIFY(evaluate(ok, QStringLiteral("Accessible.defaultButton")).toBool());
    QVERIFY(!apply->property("enabled").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

void UiDialogsTest::configurePagesMoveByKeyboardAndTabReachesEveryControl() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = openPreferences(window);
    QVERIFY(dialog != nullptr);
    QQuickItem *list = visibleNamed(window, QStringLiteral("preferencesPageList"));
    QVERIFY(list != nullptr);
    // Keyboard users start on the page list.
    QTRY_VERIFY(list->hasActiveFocus());
    QQuickItem *stack = nullptr;
    QTRY_VERIFY((stack = visibleNamed(window, QStringLiteral("preferencesPages"))) != nullptr);
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(stack->property("currentIndex").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(stack->property("currentIndex").toInt(), 2);
    QTest::keyClick(window, Qt::Key_Up);
    QTRY_COMPARE(stack->property("currentIndex").toInt(), 1);
    // Ctrl+Tab and Ctrl+Page Up and Down move between the pages too, and
    // wrap around.
    QTest::keyClick(window, Qt::Key_Tab, Qt::ControlModifier);
    QTRY_COMPARE(stack->property("currentIndex").toInt(), 2);
    QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
    QTRY_COMPARE(stack->property("currentIndex").toInt(), 4);

    // From the list, Tab reaches each page's controls, then the buttons, and
    // Space toggles a check box.
    const QList<QPair<int, QString>> controls{
        {0, QStringLiteral("libraryWatchCheckBox")}, {0, QStringLiteral("librarySortComboBox")},
        {1, QStringLiteral("spectrogramFftComboBox")}, {1, QStringLiteral("dbRangeSlider")},
        {1, QStringLiteral("channelButtonsComboBox")},
        {2, QStringLiteral("viewerFullscreenComboBox")}, {2, QStringLiteral("showLevelMeterCheckBox")},
        {3, QStringLiteral("lastFmScrobblingCheckBox")},
        {4, QStringLiteral("systemMediaControlsCheckBox")},
    };
    for (const auto &[page, name] : controls) {
        QVERIFY(dialog->setProperty("pageIndex", page));
        list->forceActiveFocus(Qt::TabFocusReason);
        QQuickItem *control = visibleNamed(window, name);
        QVERIFY2(control != nullptr, qPrintable(name));
        QVERIFY2(tabTo(window, control), qPrintable(QStringLiteral("Tab did not reach %1").arg(name)));
    }
    QQuickItem *box = visibleNamed(window, QStringLiteral("systemMediaControlsCheckBox"));
    const bool before = box->property("checked").toBool();
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(box->property("checked").toBool(), !before);
    for (const char *button : {"preferencesDefaultsButton", "preferencesOkButton", "preferencesApplyButton",
                               "preferencesCancelButton"}) {
        QQuickItem *item = visibleNamed(window, QString::fromLatin1(button));
        QVERIFY(item != nullptr);
        QVERIFY2(tabTo(window, item), button);
    }
    // Back with Shift+Tab to the list.
    QVERIFY(tabTo(window, list, 60, Qt::ShiftModifier));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    // Escape is Cancel: the change was not applied.
    QCOMPARE(bridgeOf(window)->property("systemMediaControlsEnabled").toBool(), before);
}

void UiDialogsTest::configureAppliesOnOkOrApplyAndCancelDropsTheChanges() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("libraryWatchEnabled", true));
    QObject *dialog = openPreferences(window, 0);
    QVERIFY(dialog != nullptr);
    QQuickItem *watch = nullptr;
    QTRY_VERIFY((watch = visibleNamed(window, QStringLiteral("libraryWatchCheckBox"))) != nullptr);
    QQuickItem *apply = visibleNamed(window, QStringLiteral("preferencesApplyButton"));
    QVERIFY(watch->property("checked").toBool());

    // A change waits for Apply.
    click(window, watch);
    QTRY_VERIFY(!watch->property("checked").toBool());
    QVERIFY(bridge->property("libraryWatchEnabled").toBool());
    QVERIFY(dialog->property("modified").toBool());
    QVERIFY(apply->property("enabled").toBool());
    // Changed back, there is nothing to apply.
    click(window, watch);
    QTRY_VERIFY(watch->property("checked").toBool());
    QVERIFY(!dialog->property("modified").toBool());
    QVERIFY(!apply->property("enabled").toBool());
    click(window, watch);
    QTRY_VERIFY(!watch->property("checked").toBool());
    click(window, apply);
    QTRY_VERIFY(!bridge->property("libraryWatchEnabled").toBool());
    QVERIFY(dialog->property("opened").toBool());
    QVERIFY(!watch->property("checked").toBool());
    QVERIFY(!apply->property("enabled").toBool());

    // Cancel drops what was changed since.
    click(window, watch);
    QTRY_VERIFY(watch->property("checked").toBool());
    click(window, visibleNamed(window, QStringLiteral("preferencesCancelButton")));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(!bridge->property("libraryWatchEnabled").toBool());
    // Opened again, it shows the setting as it is.
    QVERIFY(openPreferences(window, 0) != nullptr);
    QTRY_VERIFY(!watch->property("checked").toBool());
    QVERIFY(!dialog->property("modified").toBool());

    // OK applies and closes. A setting changed outside the dialog shows in
    // it while it has no change of its own.
    QVERIFY(bridge->setProperty("librarySortMode", 1));
    QQuickItem *sort = visibleNamed(window, QStringLiteral("librarySortComboBox"));
    QTRY_COMPARE(sort->property("currentIndex").toInt(), 1);
    click(window, watch);
    QTRY_VERIFY(watch->property("checked").toBool());
    click(window, visibleNamed(window, QStringLiteral("preferencesOkButton")));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(bridge->property("libraryWatchEnabled").toBool());
    QCOMPARE(bridge->property("librarySortMode").toInt(), 1);
}

void UiDialogsTest::configureDefaultsSetsThePagesBackToBeApplied() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    const QVariantMap defaults = evaluate(window, QStringLiteral("uiBridge.settingDefaults()")).toMap();
    QCOMPARE(defaults.value(QStringLiteral("dbRange")).toDouble(), 132.0);
    QVERIFY(!defaults.value(QStringLiteral("logScale")).toBool());
    // As the backend: the level meter is off for new installs.
    QCOMPARE(defaults.value(QStringLiteral("showLevelMeter")), QVariant(false));

    // At the defaults, Defaults changes nothing.
    for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
        QVERIFY2(bridge->setProperty(qPrintable(it.key()), it.value()), qPrintable(it.key()));
    }
    QObject *dialog = openPreferences(window, 1);
    QVERIFY(dialog != nullptr);
    QQuickItem *defaultsButton = visibleNamed(window, QStringLiteral("preferencesDefaultsButton"));
    QTRY_VERIFY(!defaultsButton->property("enabled").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());

    // Away from them, Defaults sets every page back, on every page, to be
    // applied with OK.
    QVERIFY(bridge->setProperty("logScale", true));
    QVERIFY(bridge->setProperty("dbRange", 90.0));
    QVERIFY(bridge->setProperty("libraryWatchEnabled", false));
    QVERIFY(bridge->setProperty("showLevelMeter", true));
    QVERIFY(openPreferences(window, 1) != nullptr);
    QTRY_VERIFY(defaultsButton->property("enabled").toBool());
    QQuickItem *logScale = visibleNamed(window, QStringLiteral("spectrogramLogScaleCheckBox"));
    QQuickItem *slider = visibleNamed(window, QStringLiteral("dbRangeSlider"));
    QVERIFY(logScale != nullptr && slider != nullptr);
    QVERIFY(logScale->property("checked").toBool());
    QCOMPARE(slider->property("value").toDouble(), 90.0);
    click(window, defaultsButton);
    QTRY_VERIFY(!logScale->property("checked").toBool());
    QCOMPARE(slider->property("value").toDouble(), 132.0);
    QCOMPARE(pendingValue(dialog, QStringLiteral("libraryWatchEnabled")).toBool(), true);
    QVERIFY(!defaultsButton->property("enabled").toBool());
    // Nothing is applied yet.
    QVERIFY(bridge->property("logScale").toBool());
    QCOMPARE(bridge->property("dbRange").toDouble(), 90.0);
    click(window, visibleNamed(window, QStringLiteral("preferencesOkButton")));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(!bridge->property("logScale").toBool());
    QCOMPARE(bridge->property("dbRange").toDouble(), 132.0);
    QVERIFY(bridge->property("libraryWatchEnabled").toBool());
    QVERIFY(!bridge->property("showLevelMeter").toBool());
}

void UiDialogsTest::enterPressesConfiguresOkButtonUnlessAControlTakesIt() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("libraryWatchEnabled", true));

    // From the page list: OK applies and closes.
    QObject *dialog = openPreferences(window, 0);
    QVERIFY(dialog != nullptr);
    QQuickItem *list = visibleNamed(window, QStringLiteral("preferencesPageList"));
    QTRY_VERIFY(list->hasActiveFocus());
    QQuickItem *watch = nullptr;
    QTRY_VERIFY((watch = visibleNamed(window, QStringLiteral("libraryWatchCheckBox"))) != nullptr);
    click(window, watch);
    QTRY_VERIFY(!watch->property("checked").toBool());
    list->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(!bridge->property("libraryWatchEnabled").toBool());

    // From a check box, with the keypad's Enter.
    QVERIFY(openPreferences(window, 0) != nullptr);
    QTRY_VERIFY(watch->isVisible());
    QVERIFY(tabTo(window, watch));
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(watch->property("checked").toBool());
    QTest::keyClick(window, Qt::Key_Enter, Qt::KeypadModifier);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(bridge->property("libraryWatchEnabled").toBool());

    // A combo box's open list takes its keys before the dialog: Enter there
    // leaves Configure open, and Escape closes only the list.
    QVERIFY(openPreferences(window, 0) != nullptr);
    QQuickItem *sort = nullptr;
    QTRY_VERIFY((sort = visibleNamed(window, QStringLiteral("librarySortComboBox"))) != nullptr);
    QVERIFY(tabTo(window, sort));
    QTest::keyClick(window, Qt::Key_Space);
    QObject *sortPopup = qvariant_cast<QObject *>(sort->property("popup"));
    QTRY_VERIFY(sortPopup->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    QVERIFY(dialog->property("visible").toBool());
    if (sortPopup->property("visible").toBool()) {
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!sortPopup->property("visible").toBool());
    }
    QVERIFY(dialog->property("visible").toBool());
    // Closed and not editable, the combo box leaves Enter to OK, which
    // applies its choice, as in a KDE dialog.
    QVERIFY(sort->setProperty("currentIndex", 1));
    QVERIFY(QMetaObject::invokeMethod(sort, "activated", Q_ARG(int, 1)));
    QTRY_VERIFY(sort->hasActiveFocus());
    QVERIFY(!sortPopup->property("visible").toBool());
    QCOMPARE(bridge->property("librarySortMode").toInt(), 0);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(bridge->property("librarySortMode").toInt(), 1);
    // So does every combo box of the pages.
    for (const auto &[page, name] : QList<QPair<int, QString>>{
             {1, QStringLiteral("spectrogramViewComboBox")}, {1, QStringLiteral("spectrogramDisplayComboBox")},
             {1, QStringLiteral("spectrogramFftComboBox")}, {1, QStringLiteral("channelButtonsComboBox")},
             {2, QStringLiteral("viewerFullscreenComboBox")}}) {
        QVERIFY(openPreferences(window, page) != nullptr);
        QQuickItem *combo = nullptr;
        QTRY_VERIFY2((combo = visibleNamed(window, name)) != nullptr, qPrintable(name));
        combo->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Enter, Qt::KeypadModifier);
        QTRY_VERIFY2(!dialog->property("visible").toBool(), qPrintable(name));
    }

    // On Cancel, Enter presses Cancel.
    QVERIFY(openPreferences(window, 0) != nullptr);
    QTRY_VERIFY(watch->isVisible());
    click(window, watch);
    QTRY_VERIFY(!watch->property("checked").toBool());
    QQuickItem *cancel = visibleNamed(window, QStringLiteral("preferencesCancelButton"));
    QVERIFY(tabTo(window, cancel));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(bridge->property("libraryWatchEnabled").toBool());
}

void UiDialogsTest::enterInATextFieldTakesTheTextNotTheDefaultButton() {
    TagEditorController editor(nullptr);
    int saves = 0;
    editor.m_executeOperation = [&saves](TagEditorController::Operation operation, const QByteArray &,
                                         const TagEditorController::RenameGate &) {
        if (operation == TagEditorController::Operation::Save) {
            ++saves;
        }
        return tagRows(2);
    };
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    // In a tag field, Enter commits the text; Save, the default button,
    // stays unpressed.
    QObject *dialog = object(window, QStringLiteral("tagEditorDialog"));
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/Album")}));
    QTRY_VERIFY(!editor.loading() && dialog->property("opened").toBool());
    editor.selectAllRows();
    QQuickItem *field = nullptr;
    QTRY_VERIFY((field = visibleNamed(window, QStringLiteral("tagEditorField_genre"))) != nullptr);
    field->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    for (const char letter : QByteArrayLiteral("Ambient")) {
        QTest::keyClick(window, letter);
    }
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("genre")), QStringLiteral("Ambient"));
    QVERIFY(editor.dirty());
    QCoreApplication::processEvents();
    QCOMPARE(saves, 0);
    QVERIFY(!editor.saving());
    QVERIFY(dialog->property("visible").toBool());
    // Out of the field, on the table, Enter presses Save.
    QQuickItem *table = named(window, QStringLiteral("tagEditorTable"));
    table->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(saves, 1);
    QTRY_VERIFY(!editor.saving());
    editor.close();
    QTRY_VERIFY(!dialog->property("visible").toBool());

    // The library folder's name: Enter in the field adds the folder once.
    QObject *nameDialog = object(window, QStringLiteral("libraryRootNameDialog"));
    QVERIFY(QMetaObject::invokeMethod(window, "openLibraryRootNameDialog", Q_ARG(QVariant, QStringLiteral("add")),
                                      Q_ARG(QVariant, QStringLiteral("/music/new")), Q_ARG(QVariant, QString())));
    QTRY_VERIFY(nameDialog->property("opened").toBool());
    QQuickItem *name = visibleNamed(window, QStringLiteral("libraryRootNameField"));
    QTRY_VERIFY(name->hasActiveFocus());
    QSignalSpy accepted(nameDialog, SIGNAL(accepted()));
    QTest::keyClick(window, Qt::Key_J);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!nameDialog->property("visible").toBool());
    QCoreApplication::processEvents();
    QCOMPARE(accepted.count(), 1);
}

void UiDialogsTest::everyConfigureSettingReachesTheBridge() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("lastFmAuthState", 2));
    QVERIFY(bridge->setProperty("lastFmUsername", QStringLiteral("listener")));
    // The title strip's setting is for viewers over the whole screen.
    QVERIFY(bridge->setProperty("viewerFullscreenMode", 1));
    QObject *dialog = openPreferences(window, 0);
    QVERIFY(dialog != nullptr);
    QTRY_VERIFY(visibleNamed(window, QStringLiteral("preferencesPages")) != nullptr);

    struct Check { int page; const char *control; const char *property; };
    const QList<Check> checks{
        {0, "libraryWatchCheckBox", "libraryWatchEnabled"},
        {1, "spectrogramLogScaleCheckBox", "logScale"},
        {1, "spectrogramFrequencyCapCheckBox", "spectrogramFrequencyCap"},
        {1, "spectrogramScaleCheckBox", "showSpectrogramScale"},
        {1, "spectrogramCrosshairCheckBox", "showSpectrogramCrosshair"},
        {1, "spectrogramFpsCheckBox", "showFps"},
        {1, "spectrogramZoomCheckBox", "spectrogramZoomEnabled"},
        {2, "showFullscreenTitleStripCheckBox", "showFullscreenTitleStrip"},
        {2, "preventDisplaySleepInFullscreenCheckBox", "preventDisplaySleepInFullscreen"},
        {2, "showLevelMeterCheckBox", "showLevelMeter"},
        {3, "lastFmScrobblingCheckBox", "lastFmScrobblingEnabled"},
        {3, "lastFmShowScrobbleProgressCheckBox", "lastFmShowScrobbleProgress"},
        {4, "systemMediaControlsCheckBox", "systemMediaControlsEnabled"},
    };
    QHash<QString, bool> expected;
    for (const Check &check : checks) {
        QVERIFY(dialog->setProperty("pageIndex", check.page));
        QQuickItem *box = nullptr;
        QTRY_VERIFY2((box = visibleNamed(window, QString::fromLatin1(check.control))) != nullptr, check.control);
        const bool before = bridge->property(check.property).toBool();
        // By the keyboard: a long page may have the box scrolled out of view.
        box->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY2(box->property("checked").toBool() == !before,
                     qPrintable(QStringLiteral("%1 at %2,%3").arg(QString::fromLatin1(check.control))
                                    .arg(sceneRect(box).x()).arg(sceneRect(box).y())));
        QCOMPARE(bridge->property(check.property).toBool(), before);
        expected.insert(QString::fromLatin1(check.property), !before);
    }

    struct Choice { int page; const char *control; int index; const char *property; QVariant value; };
    const QList<Choice> choices{
        {0, "librarySortComboBox", 1, "librarySortMode", 1},
        {1, "spectrogramViewComboBox", 1, "spectrogramViewMode", 1},
        {1, "spectrogramDisplayComboBox", 1, "spectrogramDisplayMode", 1},
        {1, "spectrogramFftComboBox", 1, "fftSize", 1024},
        {1, "channelButtonsComboBox", 2, "channelButtonsVisibility", 2},
        {2, "viewerFullscreenComboBox", 0, "viewerFullscreenMode", 0},
    };
    for (const Choice &choice : choices) {
        QVERIFY(dialog->setProperty("pageIndex", choice.page));
        QQuickItem *combo = nullptr;
        QTRY_VERIFY2((combo = visibleNamed(window, QString::fromLatin1(choice.control))) != nullptr, choice.control);
        QVERIFY(combo->setProperty("currentIndex", choice.index));
        QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, choice.index)));
        QVERIFY(bridge->property(choice.property) != choice.value);
    }
    QVERIFY(dialog->setProperty("pageIndex", 1));
    QQuickItem *slider = visibleNamed(window, QStringLiteral("dbRangeSlider"));
    QVERIFY(slider->setProperty("value", 145.0));
    QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
    QCOMPARE(visibleNamed(window, QStringLiteral("dbRangeValueLabel"))->property("text").toString(),
             QStringLiteral("145 dB"));

    click(window, visibleNamed(window, QStringLiteral("preferencesOkButton")));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        QVERIFY2(bridge->property(qPrintable(it.key())).toBool() == it.value(), qPrintable(it.key()));
    }
    for (const Choice &choice : choices) {
        QVERIFY2(bridge->property(choice.property) == choice.value, choice.property);
    }
    QCOMPARE(bridge->property("dbRange").toDouble(), 145.0);
}

void UiDialogsTest::titleStripSettingIsOnlyForTheWholeScreen() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("viewerFullscreenMode", 0));
    QVERIFY(bridge->property("showFullscreenTitleStrip").toBool());
    QObject *dialog = openPreferences(window, 2);
    QVERIFY(dialog != nullptr);
    QQuickItem *box = nullptr;
    QTRY_VERIFY((box = visibleNamed(window, QStringLiteral("showFullscreenTitleStripCheckBox"))) != nullptr);
    QCOMPARE(box->property("text").toString(), QStringLiteral("Show the title strip when the pointer moves"));
    QVERIFY(box->property("checked").toBool());
    // Fill the window: viewers in the window have corner buttons, no strip.
    QVERIFY(!box->isEnabled());

    // Fill the screen, chosen and not yet applied, enables it.
    QQuickItem *combo = visibleNamed(window, QStringLiteral("viewerFullscreenComboBox"));
    QVERIFY(combo != nullptr);
    QVERIFY(combo->setProperty("currentIndex", 1));
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1)));
    QTRY_VERIFY(box->isEnabled());
    click(window, box);
    QTRY_VERIFY(!box->property("checked").toBool());
    QVERIFY(bridge->property("showFullscreenTitleStrip").toBool());

    // Fill the window again disables it, keeping the change.
    QVERIFY(combo->setProperty("currentIndex", 0));
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 0)));
    QTRY_VERIFY(!box->isEnabled());
    QVERIFY(!box->property("checked").toBool());
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1)));
    QTRY_VERIFY(box->isEnabled());

    click(window, visibleNamed(window, QStringLiteral("preferencesOkButton")));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(!bridge->property("showFullscreenTitleStrip").toBool());
    QCOMPARE(bridge->property("viewerFullscreenMode").toInt(), 1);
}

void UiDialogsTest::configureHelpTextIsSecondaryAndSettingsKeepTheirSliderRange() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = openPreferences(window, 0);
    QVERIFY(dialog != nullptr);
    QQuickItem *help = nullptr;
    QTRY_VERIFY((help = visibleNamed(window, QStringLiteral("libraryWatchHelp"))) != nullptr);
    // The secondary colour, not the disabled one, in the base font.
    QCOMPARE(hex(help->property("color").value<QColor>()), hex(tokenOf(window, "textSecondary")));
    QCOMPARE(help->property("font").value<QFont>().pointSizeF(),
             paletteOf(window)->property("basePointSize").toReal());
    // Under the check box's text, not its indicator.
    QQuickItem *watch = visibleNamed(window, QStringLiteral("libraryWatchCheckBox"));
    QQuickItem *helpLabel = itemsNamed(help, QStringLiteral("formHelpTextLabel")).value(0);
    QVERIFY(helpLabel != nullptr);
    QCOMPARE(sceneRect(helpLabel).left(),
             sceneRect(qvariant_cast<QQuickItem *>(watch->property("contentItem"))).left());
    // Wrapped at the words: Text.Wrap.
    QCOMPARE(helpLabel->property("wrapMode").toInt(), 4);
    QQuickItem *slider = nullptr;
    QVERIFY(dialog->setProperty("pageIndex", 1));
    QTRY_VERIFY((slider = visibleNamed(window, QStringLiteral("dbRangeSlider"))) != nullptr);
    // The backend keeps 50 to 150 dB; the slider offers the same.
    QCOMPARE(slider->property("from").toDouble(), 50.0);
    QCOMPARE(slider->property("to").toDouble(), 150.0);
}

void UiDialogsTest::libraryFoldersActOnTheSelectedFolderAtOnce() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("libraryRootEntries", QVariantList{
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/music")}, {QStringLiteral("name"), QStringLiteral("Music")},
                    {QStringLiteral("displayName"), QStringLiteral("Music")}, {QStringLiteral("trackCount"), 1204}},
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/mnt/archive/flac")},
                    {QStringLiteral("displayName"), QStringLiteral("/mnt/archive/flac")},
                    {QStringLiteral("trackCount"), 38},
                    {QStringLiteral("unwatchedReason"), QStringLiteral("a network share")}},
    }));
    QObject *dialog = openPreferences(window, 0);
    QVERIFY(dialog != nullptr);
    QQuickItem *list = nullptr;
    QTRY_VERIFY((list = visibleNamed(window, QStringLiteral("libraryRootList"))) != nullptr);
    QTRY_COMPARE(itemsNamed(list, QStringLiteral("libraryRootRow")).size(), 2);
    QQuickItem *second = itemsNamed(list, QStringLiteral("libraryRootRow")).at(1);
    QVERIFY(itemsNamed(second, QStringLiteral("libraryRootUnwatchedLabel")).at(0)->isVisible());
    QVERIFY(!itemsNamed(itemsNamed(list, QStringLiteral("libraryRootRow")).at(0),
                        QStringLiteral("libraryRootUnwatchedLabel")).at(0)->isVisible());
    // The folders change at once, and the buttons act on the selected one.
    click(window, second);
    QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
    QTRY_VERIFY(list->hasActiveFocus());
    click(window, visibleNamed(window, QStringLiteral("libraryRootRemoveButton")));
    QObject *confirm = object(window, QStringLiteral("removeLibraryRootConfirmDialog"));
    QVERIFY(confirm != nullptr);
    QTRY_VERIFY(confirm->property("opened").toBool());
    QVERIFY(confirm->property("message").toString().contains(QStringLiteral("/mnt/archive/flac")));
    QVERIFY(confirm->property("details").toString().contains(QStringLiteral("38 tracks")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!confirm->property("visible").toBool());
    QVERIFY(dialog->property("opened").toBool());
    // Nothing waits for Apply.
    QVERIFY(!dialog->property("modified").toBool());
}

void UiDialogsTest::lastFmPageShowsOnlyTheButtonsThatApply() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("lastFmBuildConfigured", true));
    QObject *dialog = openPreferences(window, 3);
    QVERIFY(dialog != nullptr);
    const auto shownButtons = [&]() {
        QStringList shown;
        for (const char *name : {"lastFmConnectButton", "lastFmCompleteButton", "lastFmReconnectButton",
                                 "lastFmDisconnectButton"}) {
            QQuickItem *button = named(window, QString::fromLatin1(name));
            if (button != nullptr && button->isVisible()) {
                shown.push_back(QString::fromLatin1(name));
            }
        }
        return shown;
    };
    QTRY_COMPARE(shownButtons(), QStringList{QStringLiteral("lastFmConnectButton")});
    QVERIFY(!visibleNamed(window, QStringLiteral("lastFmShowScrobbleProgressCheckBox")));
    QVERIFY(bridge->setProperty("lastFmAuthState", 1));
    QTRY_COMPARE(shownButtons(), (QStringList{QStringLiteral("lastFmCompleteButton"),
                                              QStringLiteral("lastFmDisconnectButton")}));
    QVERIFY(bridge->setProperty("lastFmAuthState", 2));
    QVERIFY(bridge->setProperty("lastFmUsername", QStringLiteral("listener")));
    QTRY_COMPARE(shownButtons(), (QStringList{QStringLiteral("lastFmReconnectButton"),
                                              QStringLiteral("lastFmDisconnectButton")}));
    QVERIFY(visibleNamed(window, QStringLiteral("lastFmShowScrobbleProgressCheckBox")));
    QCOMPARE(visibleNamed(window, QStringLiteral("lastFmAccountLabel"))->property("text").toString(),
             QStringLiteral("Connected as listener"));
}

void UiDialogsTest::aboutShowsTheIconVersionTabsAndCopiesTheVersion() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = object(window, QStringLiteral("aboutDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QQuickItem *icon = visibleNamed(window, QStringLiteral("aboutIcon"));
    QVERIFY(icon != nullptr);
    QCOMPARE(icon->property("source").toString(), QStringLiteral("qrc:/icons/hicolor/48x48/apps/ferrous.svg"));
    QTRY_VERIFY(icon->property("valid").toBool());
    QCOMPARE(icon->width(), 48.0);
    QCOMPARE(visibleNamed(window, QStringLiteral("aboutName"))->property("text").toString(), QStringLiteral("Ferrous"));
    QCOMPARE(visibleNamed(window, QStringLiteral("aboutVersion"))->property("text").toString(),
             QStringLiteral("Version 0.3.0"));
    QVERIFY(visibleNamed(window, QStringLiteral("aboutHomepageLink")) != nullptr);
    QVERIFY(visibleNamed(window, QStringLiteral("aboutBugReportLink")) != nullptr);
    QQuickItem *tabs = visibleNamed(window, QStringLiteral("aboutTabs"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->property("count").toInt(), 4);
    QTRY_VERIFY(qvariant_cast<QQuickItem *>(tabs->property("currentItem"))->hasActiveFocus());
    QQuickItem *pages = visibleNamed(window, QStringLiteral("aboutPages"));
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE(pages->property("currentIndex").toInt(), 1);

    // Copy and Close: on the left and the right of the button row.
    QQuickItem *copy = visibleNamed(window, QStringLiteral("aboutCopyVersionButton"));
    QQuickItem *close = visibleNamed(window, QStringLiteral("aboutCloseButton"));
    QVERIFY(copy != nullptr && close != nullptr);
    QVERIFY(sceneRect(copy).right() < sceneRect(close).left());
    QGuiApplication::clipboard()->clear();
    click(window, copy);
    QTRY_VERIFY(QGuiApplication::clipboard()->text().startsWith(QStringLiteral("Ferrous 0.3.0\n")));
    QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("Qt ")));
    click(window, close);
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

void UiDialogsTest::recentIssuesIsAPopupAboveTheStatusBar() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    report(window, 1, 0, kStaticPath, kEndsEarly);
    QObject *popup = object(window, QStringLiteral("recentErrorsDialog"));
    QVERIFY(popup != nullptr);
    QVERIFY(openIssues(window));
    // 460 px, 6 px from the window's right edge and just above the status bar.
    QCOMPARE(popup->property("width").toReal(), 460.0);
    QCOMPARE(popup->property("x").toReal() + popup->property("width").toReal(), window->width() - 6.0);
    QQuickItem *statusBar = named(window, QStringLiteral("statusBar"));
    QVERIFY(statusBar != nullptr);
    const qreal bottom = popup->property("y").toReal() + popup->property("height").toReal();
    QVERIFY2(bottom <= sceneRect(statusBar).top() && bottom >= sceneRect(statusBar).top() - 8,
             qPrintable(QStringLiteral("%1 vs %2").arg(bottom).arg(sceneRect(statusBar).top())));
    // No dim over the window, and the issues button shows it open.
    QVERIFY(!popup->property("dim").toBool());
    QTRY_VERIFY(statusBar->property("issuesOpen").toBool());
    // The header: the title, how many are new, and a flat Clear All.
    QQuickItem *header = visibleNamed(window, QStringLiteral("errorHistoryHeader"));
    QVERIFY(header != nullptr);
    QCOMPARE(header->height(), 32.0);
    QCOMPARE(visibleNamed(window, QStringLiteral("errorHistoryTitle"))->property("text").toString(),
             QStringLiteral("Recent Issues"));
    QQuickItem *newCount = visibleNamed(window, QStringLiteral("errorHistoryNewCount"));
    QCOMPARE(newCount->property("text").toString(), QStringLiteral("1 new"));
    QCOMPARE(hex(newCount->property("color").value<QColor>()), hex(tokenOf(window, "textTertiary")));
    QQuickItem *clear = visibleNamed(window, QStringLiteral("errorHistoryClearButton"));
    QVERIFY(clear->property("flat").toBool());
    QQuickItem *diagnostics = visibleNamed(window, QStringLiteral("errorHistoryDiagnosticsButton"));
    QCOMPARE(diagnostics->property("text").toString(), QStringLiteral("Open Diagnostics\u2026"));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
}

void UiDialogsTest::recentIssuesRowsSplitTheMessageAndOfferOneAction() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("queueRows", queueRowsOf({QStringLiteral("/music/a.flac"), kStaticPath})));
    QVERIFY(bridge->setProperty("queueLength", 2));
    report(window, 6, 1, QString(), QStringLiteral("The settings could not be saved: disk full"));
    report(window, 1, 0, kStaticPath, kEndsEarly);
    QVERIFY(openIssues(window));
    QList<QQuickItem *> rows;
    QTRY_COMPARE((rows = itemsNamed(window->contentItem(), QStringLiteral("errorHistoryRow"))).size(), 2);
    // The decode issue, as the spec words it: the pane's message matches it.
    QQuickItem *first = rows[0];
    QQuickItem *heading = itemsNamed(first, QStringLiteral("errorHistoryHeading")).at(0);
    QCOMPARE(heading->property("text").toString(), QStringLiteral("04 - Static.flac ends early"));
    QCOMPARE(heading->property("font").value<QFont>().weight(), QFont::DemiBold);
    QQuickItem *description = itemsNamed(first, QStringLiteral("errorHistoryMessage")).at(0);
    QCOMPARE(description->property("text").toString(),
             QStringLiteral("The last FLAC frame is cut off, so playback stops at 4:02."));
    QCOMPARE(hex(description->property("color").value<QColor>()), hex(tokenOf(window, "textSecondary")));
    const qreal small = paletteOf(window)->property("smallPointSize").toReal();
    QCOMPARE(description->property("font").value<QFont>().pointSizeF(), small);
    QQuickItem *time = itemsNamed(first, QStringLiteral("errorHistoryTime")).at(0);
    QVERIFY(QRegularExpression(QStringLiteral("^\\d\\d:\\d\\d$")).match(time->property("text").toString()).hasMatch());
    QCOMPARE(hex(time->property("color").value<QColor>()), hex(tokenOf(window, "textTertiary")));
    QVERIFY(sceneRect(time).right() > sceneRect(heading).right());
    // The file is named in the title, so its path does not repeat.
    QVERIFY(!itemsNamed(first, QStringLiteral("errorHistorySubject")).at(0)->isVisible());
    QQuickItem *glyph = itemsNamed(first, QStringLiteral("errorHistoryGlyph")).at(0);
    QCOMPARE(glyph->width(), 16.0);
    QCOMPARE(hex(glyph->property("color").value<QColor>()), hex(tokenOf(window, "negative")));
    QQuickItem *action = itemsNamed(first, QStringLiteral("errorHistoryActionButton")).at(0);
    QVERIFY(action->isVisible());
    QCOMPARE(action->property("text").toString(), QStringLiteral("Show in Queue"));
    QVERIFY(action->property("flat").toBool());
    QCOMPARE(hex(qvariant_cast<QObject *>(action->property("contentItem"))->property("color").value<QColor>()), hex(tokenOf(window, "link")));
    // One sentence: the category is the title; no action for settings.
    QQuickItem *second = rows[1];
    QCOMPARE(itemsNamed(second, QStringLiteral("errorHistoryHeading")).at(0)->property("text").toString(),
             QStringLiteral("Settings and saving"));
    QCOMPARE(itemsNamed(second, QStringLiteral("errorHistoryMessage")).at(0)->property("text").toString(),
             QStringLiteral("The settings could not be saved: disk full"));
    QVERIFY(!itemsNamed(second, QStringLiteral("errorHistoryActionButton")).at(0)->isVisible());
    // A file that left the queue is shown in the library instead.
    QVERIFY(bridge->setProperty("queueRows", queueRowsOf({QStringLiteral("/music/a.flac")})));
    QVERIFY(bridge->setProperty("queueLength", 1));
    QTRY_COMPARE(action->property("text").toString(), QStringLiteral("Show in Library"));
}

void UiDialogsTest::recentIssuesActionsShowTheFileOrOpenTheSettings() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVariantList queueRows;
    QStringList paths;
    for (int i = 0; i < 5; ++i) {
        const QString path = i == 3 ? kStaticPath : QStringLiteral("/music/%1.flac").arg(i);
        paths.push_back(path);
        queueRows.push_back(QVariantMap{{QStringLiteral("title"), QStringLiteral("Track %1").arg(i)},
                                        {QStringLiteral("path"), path},
                                        {QStringLiteral("lengthText"), QStringLiteral("1:00")}});
    }
    QVERIFY(bridge->setProperty("queueRows", queueRows));
    QVERIFY(bridge->setProperty("queueLength", 5));
    QObject *popup = object(window, QStringLiteral("recentErrorsDialog"));
    QObject *queueController = object(window, QStringLiteral("queueController"));
    QObject *preferences = object(window, QStringLiteral("preferencesDialog"));

    // Show in Queue selects the file's row, with the keyboard on the queue.
    report(window, 1, 0, kStaticPath, kEndsEarly);
    QVERIFY(openIssues(window));
    QQuickItem *action = nullptr;
    QTRY_VERIFY((action = visibleNamed(window, QStringLiteral("errorHistoryActionButton"))) != nullptr);
    click(window, action);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCOMPARE(evaluate(queueController, QStringLiteral("selectedIndices")).toList(), QVariantList{3});
    QTRY_VERIFY(named(window, QStringLiteral("playlistView"))->hasActiveFocus());

    // A file not in the queue: shown in the library.
    QVERIFY(QMetaObject::invokeMethod(object(window, QStringLiteral("errorController")), "clear"));
    report(window, 1, 1, QStringLiteral("/music/gone.flac"), QStringLiteral("The file could not be opened."));
    QVERIFY(openIssues(window));
    QTRY_VERIFY((action = visibleNamed(window, QStringLiteral("errorHistoryActionButton"))) != nullptr);
    QCOMPARE(action->property("text").toString(), QStringLiteral("Show in Library"));
    click(window, action);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCOMPARE(bridge->property("shownInLibrary").toStringList(), QStringList{QStringLiteral("/music/gone.flac")});

    // Reopened while it fades out, the action chosen before still runs, once.
    QVERIFY(QMetaObject::invokeMethod(object(window, QStringLiteral("errorController")), "clear"));
    report(window, 1, 1, QStringLiteral("/music/gone-again.flac"), QStringLiteral("The file could not be opened."));
    QVERIFY(openIssues(window));
    QTRY_VERIFY((action = visibleNamed(window, QStringLiteral("errorHistoryActionButton"))) != nullptr);
    click(window, action);
    QVERIFY2(popup->property("visible").toBool(), "the popup closed without fading out, so this checks nothing");
    QVERIFY(QMetaObject::invokeMethod(popup, "open"));
    const QStringList bothShown{QStringLiteral("/music/gone.flac"), QStringLiteral("/music/gone-again.flac")};
    QTRY_COMPARE(bridge->property("shownInLibrary").toStringList(), bothShown);
    QTRY_VERIFY(popup->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCOMPARE(bridge->property("shownInLibrary").toStringList(), bothShown);

    // A library notice opens the library settings; a Last.fm one, Last.fm's.
    const QList<QPair<int, int>> settings{{2, 0}, {5, 3}};
    for (const auto &[category, page] : settings) {
        QVERIFY(QMetaObject::invokeMethod(object(window, QStringLiteral("errorController")), "clear"));
        report(window, category, 0, QString(), QStringLiteral("Something to look at. It explains itself."));
        QVERIFY(openIssues(window));
        QTRY_VERIFY((action = visibleNamed(window, QStringLiteral("errorHistoryActionButton"))) != nullptr);
        QVERIFY(action->property("text").toString().endsWith(QStringLiteral("Settings\u2026")));
        click(window, action);
        QTRY_VERIFY(preferences->property("opened").toBool());
        QCOMPARE(preferences->property("pageIndex").toInt(), page);
        QVERIFY(!popup->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!preferences->property("visible").toBool());
    }
}

void UiDialogsTest::recentIssuesClearAllDiagnosticsAndKeyboard() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    report(window, 2, 0, QString(), QStringLiteral("Archive isn\u2019t watched. It is rescanned every 30 minutes instead."));
    report(window, 1, 0, kStaticPath, kEndsEarly);
    QObject *popup = object(window, QStringLiteral("recentErrorsDialog"));
    QVERIFY(openIssues(window));
    // The list takes the keyboard first; Tab goes on to the actions, Clear
    // All and Open Diagnostics.
    QQuickItem *view = visibleNamed(window, QStringLiteral("errorHistoryView"));
    QTRY_VERIFY(view->hasActiveFocus());
    QList<QQuickItem *> actions;
    for (QQuickItem *item : itemsNamed(window->contentItem(), QStringLiteral("errorHistoryActionButton"))) {
        if (item->isVisible()) {
            actions.push_back(item);
        }
    }
    QCOMPARE(actions.size(), 2);
    for (QQuickItem *item : actions) {
        QVERIFY(tabTo(window, item, 10));
    }
    QQuickItem *diagnostics = visibleNamed(window, QStringLiteral("errorHistoryDiagnosticsButton"));
    QVERIFY(tabTo(window, diagnostics, 10));
    QVERIFY(tabTo(window, visibleNamed(window, QStringLiteral("errorHistoryClearButton")), 10));
    // Space presses the focused button, not Play/Pause.
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCOMPARE(object(window, QStringLiteral("errorController"))->property("count").toInt(), 0);
    QVERIFY(!named(window, QStringLiteral("statusBarErrorsButton"))->isVisible());

    // Open Diagnostics… closes the popup and opens the log.
    report(window, 1, 1, QString(), QStringLiteral("Something failed."));
    QVERIFY(openIssues(window));
    QTRY_VERIFY((diagnostics = visibleNamed(window, QStringLiteral("errorHistoryDiagnosticsButton"))) != nullptr);
    click(window, diagnostics);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QObject *diagnosticsDialog = nullptr;
    for (QObject *candidate : window->findChildren<QObject *>()) {
        if (QString::fromLatin1(candidate->metaObject()->className()).startsWith(QStringLiteral("DiagnosticsDialog"))) {
            diagnosticsDialog = candidate;
        }
    }
    QVERIFY(diagnosticsDialog != nullptr);
    QTRY_VERIFY(diagnosticsDialog->property("opened").toBool());
}

void UiDialogsTest::recentIssuesAreSeenWhenThePopupCloses() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *errors = object(window, QStringLiteral("errorController"));
    report(window, 1, 0, kStaticPath, kEndsEarly);
    QCOMPARE(errors->property("unseenCount").toInt(), 1);
    QObject *popup = object(window, QStringLiteral("recentErrorsDialog"));
    // The status bar's button opens it; a press outside closes it, also on
    // that button, which then does not open it again.
    QQuickItem *button = named(window, QStringLiteral("statusBarErrorsButton"));
    QVERIFY(button != nullptr && button->isVisible());
    click(window, button);
    QTRY_VERIFY(popup->property("opened").toBool());
    // New while it is open, and seen once it closes.
    QCOMPARE(errors->property("unseenCount").toInt(), 1);
    report(window, 1, 1, QString(), QStringLiteral("Another failure."));
    QTRY_COMPARE(itemsNamed(window->contentItem(), QStringLiteral("errorHistoryRow")).size(), 2);
    QCOMPARE(errors->property("unseenCount").toInt(), 2);
    click(window, button);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCoreApplication::processEvents();
    QVERIFY(!popup->property("visible").toBool());
    QCOMPARE(errors->property("unseenCount").toInt(), 0);
    // Opened again, no title is semibold.
    QVERIFY(openIssues(window));
    QVERIFY(!visibleNamed(window, QStringLiteral("errorHistoryNewCount")));
    for (QQuickItem *heading : itemsNamed(window->contentItem(), QStringLiteral("errorHistoryHeading"))) {
        QCOMPARE(heading->property("font").value<QFont>().weight(), QFont::Normal);
    }
}

void UiDialogsTest::thePanesEndsEarlyLinkOpensItsIssue() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("queueRows", queueRowsOf({QStringLiteral("/music/a.flac"), kStaticPath})));
    QVERIFY(bridge->setProperty("queueLength", 2));
    // The backend's warning for a cut-off file, as the analysis reports it.
    QVERIFY(QMetaObject::invokeMethod(bridge, "bridgeError", Q_ARG(QString, kEndsEarly), Q_ARG(int, 1),
                                      Q_ARG(int, 0), Q_ARG(QString, kStaticPath)));
    // The pane's message after the cut, and its way to Recent Issues.
    QVERIFY(bridge->setProperty("currentTrackPath", kStaticPath));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Playing")));
    QVERIFY(bridge->setProperty("waveformCoverageSeconds", 242.6));
    QVERIFY(bridge->setProperty("waveformEndsEarly", true));
    QQuickItem *layer = window->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    QVERIFY(layer != nullptr);
    QVERIFY(layer->setProperty("markerX", 200.0));
    QQuickItem *text = nullptr;
    QTRY_VERIFY((text = visibleNamed(window, QStringLiteral("plotEndsEarlyText"))) != nullptr);
    QCOMPARE(text->property("text").toString(), QStringLiteral("The file ends at 4:02."));
    QQuickItem *link = visibleNamed(window, QStringLiteral("plotEndsEarlyLink"));
    QVERIFY(link != nullptr);
    link->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    QObject *popup = object(window, QStringLiteral("recentErrorsDialog"));
    QTRY_VERIFY(popup->property("opened").toBool());
    QList<QQuickItem *> rows;
    QTRY_COMPARE((rows = itemsNamed(window->contentItem(), QStringLiteral("errorHistoryRow"))).size(), 1);
    // The spec's title and description, with the time the pane gives in it.
    QCOMPARE(itemsNamed(rows[0], QStringLiteral("errorHistoryHeading")).at(0)->property("text").toString(),
             QStringLiteral("04 - Static.flac ends early"));
    QCOMPARE(itemsNamed(rows[0], QStringLiteral("errorHistoryMessage")).at(0)->property("text").toString(),
             QStringLiteral("The last FLAC frame is cut off, so playback stops at 4:02."));
    QCOMPARE(hex(itemsNamed(rows[0], QStringLiteral("errorHistoryGlyph")).at(0)->property("color").value<QColor>()),
             hex(tokenOf(window, "negative")));
    // One action.
    QList<QQuickItem *> actions;
    for (QQuickItem *action : itemsNamed(rows[0], QStringLiteral("errorHistoryActionButton"))) {
        if (action->isVisible()) {
            actions.push_back(action);
        }
    }
    QCOMPARE(actions.size(), 1);
    QCOMPARE(actions.first()->property("text").toString(), QStringLiteral("Show in Queue"));
}

void UiDialogsTest::recentIssuesFollowAQueueReplacedWithOneOfTheSameLength() {
    // The bridge's own rows, whose count stays the same when another queue
    // of one track replaces this one.
    QueueRowsModel model;
    const auto rowOf = [](const QString &path) {
        QueueRowData row;
        row.title = QFileInfo(path).completeBaseName();
        row.path = path;
        row.lengthText = QStringLiteral("4:18");
        return row;
    };
    QVERIFY(model.setRows({rowOf(kStaticPath)}));
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("queueRows", QVariant::fromValue<QObject *>(&model)));
    QVERIFY(bridge->setProperty("queueLength", 1));
    report(window, 1, 0, kStaticPath, kEndsEarly);
    QVERIFY(openIssues(window));
    QQuickItem *action = nullptr;
    QTRY_VERIFY((action = visibleNamed(window, QStringLiteral("errorHistoryActionButton"))) != nullptr);
    QCOMPARE(action->property("text").toString(), QStringLiteral("Show in Queue"));

    // Replaced from outside while the popup is open: the action follows.
    QVERIFY(model.setRows({rowOf(QStringLiteral("/music/other.flac"))}));
    QTRY_COMPARE(action->property("text").toString(), QStringLiteral("Show in Library"));
    // And back again, by an edit of the row.
    QVERIFY(model.updateRow(0, rowOf(kStaticPath)));
    QTRY_COMPARE(action->property("text").toString(), QStringLiteral("Show in Queue"));

    // The file leaves the queue at the very moment of the click: the action
    // does nothing it cannot, and the popup stays with the one that fits.
    QObject *popup = object(window, QStringLiteral("recentErrorsDialog"));
    const QSignalBlocker quiet(&model);
    QVERIFY(model.setRows({rowOf(QStringLiteral("/music/other.flac"))}));
    QCOMPARE(action->property("text").toString(), QStringLiteral("Show in Queue"));
    QVERIFY(emitClicked(action));
    QCoreApplication::processEvents();
    QVERIFY(popup->property("visible").toBool());
    QCOMPARE(action->property("text").toString(), QStringLiteral("Show in Library"));
    QCOMPARE(evaluate(object(window, QStringLiteral("queueController")), QStringLiteral("selectedIndices")).toList(),
             QVariantList{});
}

void UiDialogsTest::theQueuesPathIndexFollowsEveryEdit() {
    const auto rowOf = [](const QString &path) {
        QueueRowData row;
        row.path = path;
        return row;
    };
    const QString a = QStringLiteral("/music/a.flac");
    const QString b = QStringLiteral("/music/b.flac");
    const QString c = QStringLiteral("/music/c.flac");
    QueueRowsModel model;
    QVERIFY(!model.containsPath(a));
    QVERIFY(model.setRows({rowOf(a), rowOf(b), rowOf(a)}));
    QVERIFY(model.containsPath(a) && model.containsPath(b) && !model.containsPath(c));
    // A path twice: removing one of its rows keeps it.
    QVERIFY(model.removeRowsAt(0, 1));
    QVERIFY(model.containsPath(a));
    QVERIFY(model.removeRowsAt(1, 1));
    QVERIFY(!model.containsPath(a));
    QVERIFY(model.insertRowsAt(1, {rowOf(c), rowOf(a)}));
    QVERIFY(model.containsPath(a) && model.containsPath(c));
    // Moves change nothing; an edited row's path moves with it.
    QVERIFY(model.moveRow(0, 2));
    QVERIFY(model.moveRows(0, 2, 1));
    QVERIFY(model.containsPath(a) && model.containsPath(b) && model.containsPath(c));
    for (int i = 0; i < model.rowCount(); ++i) {
        if (model.rowAt(i)->path == b) {
            QVERIFY(model.updateRow(i, rowOf(QStringLiteral("/music/d.flac"))));
        }
    }
    QVERIFY(!model.containsPath(b));
    QVERIFY(model.containsPath(QStringLiteral("/music/d.flac")));
    // Cleared, as a new empty queue or by removing every row.
    QVERIFY(model.removeRowsAt(0, model.rowCount()));
    QVERIFY(!model.containsPath(a) && !model.containsPath(c));
    QVERIFY(model.setRows({rowOf(a)}));
    QVERIFY(model.setRows({}));
    QVERIFY(!model.containsPath(a));
}

void UiDialogsTest::recentIssuesLookUpALargeQueueAtTheSameCost() {
    const auto rowsOf = [](int first, int count) {
        QVector<QueueRowData> rows;
        rows.reserve(count);
        for (int i = first; i < first + count; ++i) {
            QueueRowData row;
            row.path = QStringLiteral("/music/%1.flac").arg(i);
            row.title = QStringLiteral("Track %1").arg(i);
            rows.push_back(std::move(row));
        }
        return rows;
    };
    QueueRowsModel model;
    QVERIFY(model.setRows(rowsOf(0, 100)));
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge->setProperty("queueRows", QVariant::fromValue<QObject *>(&model)));
    QVERIFY(bridge->setProperty("queueLength", model.rowCount()));
    // As many issues as the history keeps, about files past the end of the
    // queue, the worst case for a scan.
    QObject *errors = object(window, QStringLiteral("errorController"));
    const int limit = errors->property("historyLimit").toInt();
    for (int i = 0; i < limit; ++i) {
        report(window, 1, 1, QStringLiteral("/music/%1.flac").arg(20000 + i), QStringLiteral("Couldn't play it."));
    }
    QObject *popup = object(window, QStringLiteral("recentErrorsDialog"));
    QVERIFY(QMetaObject::invokeMethod(popup, "open"));
    QTRY_VERIFY(popup->property("opened").toBool());
    // The best of a few refreshes, so that a busy machine does not decide.
    const auto refreshTime = [&]() {
        qint64 best = std::numeric_limits<qint64>::max();
        for (int run = 0; run < 5; ++run) {
            QElapsedTimer timer;
            timer.start();
            for (int repeat = 0; repeat < 20; ++repeat) {
                QMetaObject::invokeMethod(popup, "refreshQueuedSubjects");
            }
            best = std::min(best, timer.nsecsElapsed());
        }
        return best;
    };
    const qint64 small = refreshTime();
    QVERIFY(evaluate(popup, QStringLiteral("Object.keys(queuedSubjects).length")).toInt() == 0);

    // 10,000 rows, then a bulk insert while the popup is open that brings in
    // the issues' files.
    QVERIFY(model.setRows(rowsOf(0, 10000)));
    QVERIFY(bridge->setProperty("queueLength", model.rowCount()));
    QVERIFY(model.insertRowsAt(5000, rowsOf(20000, limit)));
    QVERIFY(bridge->setProperty("queueLength", model.rowCount()));
    QTRY_COMPARE(evaluate(popup, QStringLiteral("Object.keys(queuedSubjects).length")).toInt(), limit);
    QList<QQuickItem *> actions;
    QTRY_VERIFY((actions = itemsNamed(window->contentItem(), QStringLiteral("errorHistoryActionButton"))).size()
                == limit);
    for (QQuickItem *action : actions) {
        QCOMPARE(action->property("text").toString(), QStringLiteral("Show in Queue"));
    }
    const qint64 large = refreshTime();
    // A scan of the queue for each issue would take about a hundred times
    // as long as with 100 rows; the index takes about the same time.
    QVERIFY2(large < small * 10 + 2'000'000,
             qPrintable(QStringLiteral("%1 ns with 100 rows, %2 ns with %3 rows")
                            .arg(small).arg(large).arg(model.rowCount())));
    // And they leave again, half of them.
    QVERIFY(model.removeRowsAt(5000, limit / 2));
    QTRY_COMPARE(evaluate(popup, QStringLiteral("Object.keys(queuedSubjects).length")).toInt(), limit - limit / 2);
}

void UiDialogsTest::editTagsMarksAChangedFieldAndColumnAndReverts() {
    TagEditorController editor(nullptr);
    editor.m_executeOperation = [](TagEditorController::Operation, const QByteArray &,
                                   const TagEditorController::RenameGate &) { return tagRows(8); };
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = object(window, QStringLiteral("tagEditorDialog"));
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/Album")}));
    QTRY_VERIFY(!editor.loading() && dialog->property("opened").toBool());
    editor.selectAllRows();
    QQuickItem *label = nullptr;
    QTRY_VERIFY((label = visibleNamed(window, QStringLiteral("tagEditorLabel_genre"))) != nullptr);
    QQuickItem *field = visibleNamed(window, QStringLiteral("tagEditorField_genre"));
    QQuickItem *revert = named(window, QStringLiteral("tagEditorRevert_genre"));
    QQuickItem *column = named(window, QStringLiteral("tagEditorColumn_genre"));
    QVERIFY(field != nullptr && revert != nullptr && column != nullptr);
    QQuickItem *columnLabel = itemsNamed(column, QStringLiteral("tagEditorColumnLabel")).at(0);
    QQuickItem *columnRevert = named(window, QStringLiteral("tagEditorColumnRevert_genre"));
    QCOMPARE(label->property("text").toString(), QStringLiteral("Genre:"));
    QCOMPARE(label->property("font").value<QFont>().weight(), QFont::Normal);
    QVERIFY(!revert->isVisible());
    QCOMPARE(columnLabel->property("font").value<QFont>().weight(), QFont::Normal);
    QVERIFY(!columnRevert->isVisible());

    // Typed and committed, the field's label turns semibold with a revert
    // button, and so does the column's header; the cells stay regular and
    // untinted.
    field->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    for (const char letter : QByteArrayLiteral("Ambient")) {
        QTest::keyClick(window, letter);
    }
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_COMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("genre")), QStringLiteral("Ambient"));
    QTRY_COMPARE(label->property("font").value<QFont>().weight(), QFont::DemiBold);
    QVERIFY(revert->isVisible());
    QCOMPARE(evaluate(revert, QStringLiteral("ToolTip.text")).toString(), QStringLiteral("Revert to Classical"));
    QCOMPARE(columnLabel->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(hex(columnLabel->property("color").value<QColor>()), hex(tokenOf(window, "text")));
    QVERIFY(columnRevert->isVisible());
    QQuickItem *status = visibleNamed(window, QStringLiteral("tagEditorStatusLabel"));
    QTRY_COMPARE(status->property("text").toString(),
                 QStringLiteral("8 files selected. Genre changed from Classical."));
    editor.setSelectedRows({});
    const QList<QQuickItem *> rows = itemsNamed(window->contentItem(), QStringLiteral("tagEditorRow"));
    QVERIFY(!rows.isEmpty());
    for (QQuickItem *row : rows) {
        QQuickItem *tint = itemsNamed(row, QStringLiteral("rowBackgroundTint")).at(0);
        QVERIFY(!tint->isVisible() || row->property("selected").toBool());
    }

    // Only some rows selected: the field reverts those, the column every row.
    editor.setSelectedRows({0, 1});
    QString failure;
    QVERIFY2(clickButtonWhenReachable(window, revert, &failure), qPrintable(failure));
    // A click reverts at once.
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("genre")), QStringLiteral("Classical"));
    QCOMPARE(editor.m_tableModel.fieldValue(1, QStringLiteral("genre")), QStringLiteral("Classical"));
    QCOMPARE(editor.m_tableModel.fieldValue(2, QStringLiteral("genre")), QStringLiteral("Ambient"));
    QTRY_VERIFY(!revert->isVisible());
    QCOMPARE(field->property("text").toString(), QStringLiteral("Classical"));
    QVERIFY(columnRevert->isVisible());
    QVERIFY(editor.dirty());
    QVERIFY2(clickButtonWhenReachable(window, columnRevert, &failure), qPrintable(failure));
    QCOMPARE(editor.m_tableModel.fieldValue(7, QStringLiteral("genre")), QStringLiteral("Classical"));
    QTRY_VERIFY(!columnRevert->isVisible());
    QCOMPARE(columnLabel->property("font").value<QFont>().weight(), QFont::Normal);
    QVERIFY(!editor.dirty());
    editor.close();
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

// The column's Revert shows as soon as an edit changes the column, but its
// header row takes it back only when it is next laid out: a click before
// that went to the column's label (#516).
void UiDialogsTest::editTagsColumnRevertIsClickedOnceTheHeaderIsLaidOut() {
    TagEditorController editor(nullptr);
    editor.m_executeOperation = [](TagEditorController::Operation, const QByteArray &,
                                   const TagEditorController::RenameGate &) { return tagRows(8); };
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = object(window, QStringLiteral("tagEditorDialog"));
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/Album")}));
    QTRY_VERIFY(!editor.loading() && dialog->property("opened").toBool());
    QQuickItem *columnRevert = named(window, QStringLiteral("tagEditorColumnRevert_genre"));
    QVERIFY(columnRevert != nullptr);
    QTRY_VERIFY(!layoutPending(columnRevert));
    QVERIFY(!columnRevert->isVisible());

    // Changed with no event processed since: shown, but not yet laid out.
    editor.applyBulkFieldToRows({0, 1, 2, 3, 4, 5, 6, 7}, QStringLiteral("genre"), QStringLiteral("Ambient"));
    QVERIFY(columnRevert->isVisible());
    QVERIFY2(layoutPending(columnRevert), "the header row was laid out before the test could click");
    QString failure;
    QVERIFY2(clickButtonWhenReachable(window, columnRevert, &failure), qPrintable(failure));
    for (int row = 0; row < 8; ++row) {
        QCOMPARE(editor.m_tableModel.fieldValue(row, QStringLiteral("genre")), QStringLiteral("Classical"));
    }
    QTRY_VERIFY(!columnRevert->isVisible());
    QVERIFY(!editor.dirty());
    editor.close();
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

void UiDialogsTest::editTagsTableHeaderUsesTheQueuesHeaderStyle() {
    TagEditorController editor(nullptr);
    editor.m_executeOperation = [](TagEditorController::Operation, const QByteArray &,
                                   const TagEditorController::RenameGate &) { return tagRows(3); };
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = object(window, QStringLiteral("tagEditorDialog"));
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/Album")}));
    QTRY_VERIFY(!editor.loading() && dialog->property("opened").toBool());
    // The window's title in the standard header, with no heading of its own
    // and no text-glyph close button.
    QCOMPARE(dialog->property("title").toString(), QStringLiteral("Edit Tags"));
    QQuickItem *closeButton = visibleNamed(window, QStringLiteral("tagEditorCloseButton"));
    QVERIFY(closeButton != nullptr);
    QCOMPARE(evaluate(closeButton, QStringLiteral("icon.name")).toString().startsWith(QStringLiteral("window-close")), true);
    QQuickItem *queueLabel = named(window, QStringLiteral("queueHeader_artist"));
    QVERIFY(queueLabel != nullptr);
    QQuickItem *header = nullptr;
    QTRY_VERIFY((header = visibleNamed(window, QStringLiteral("tagEditorTableHeader"))) != nullptr);
    QCOMPARE(header->height(), paletteOf(window)->property("rowHeight").toReal());
    const QList<QQuickItem *> labels = itemsNamed(header, QStringLiteral("tagEditorColumnLabel"));
    QCOMPARE(labels.size(), 11);
    const QFont queueFont = queueLabel->property("font").value<QFont>();
    for (QQuickItem *label : labels) {
        const QFont font = label->property("font").value<QFont>();
        QCOMPARE(font.pointSizeF(), queueFont.pointSizeF());
        QCOMPARE(font.weight(), queueFont.weight());
        QCOMPARE(hex(label->property("color").value<QColor>()), hex(queueLabel->property("color").value<QColor>()));
    }
    QCOMPARE(labels.at(0)->property("text").toString(), QStringLiteral("#"));
    QCOMPARE(labels.at(1)->property("text").toString(), QStringLiteral("Title"));
    // The last column takes the room that is left, so nothing is cut off.
    QQuickItem *table = named(window, QStringLiteral("tagEditorTable"));
    QQuickItem *last = named(window, QStringLiteral("tagEditorColumn_status"));
    QVERIFY(sceneRect(last).right() >= sceneRect(table).right() - 8.5 - 0.5
            || table->property("contentWidth").toReal() > table->width());
    editor.close();
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

void UiDialogsTest::editTagsToolsMenuHoldsTheToolsAndCaseChanges() {
    TagEditorController editor(nullptr);
    editor.m_executeOperation = [](TagEditorController::Operation, const QByteArray &,
                                   const TagEditorController::RenameGate &) { return tagRows(2); };
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = object(window, QStringLiteral("tagEditorDialog"));
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/Album")}));
    QTRY_VERIFY(!editor.loading() && dialog->property("opened").toBool());
    editor.selectAllRows();
    editor.applyBulkField(QStringLiteral("title"), QStringLiteral("the long way home"));
    QQuickItem *tools = visibleNamed(window, QStringLiteral("tagEditorToolsButton"));
    QQuickItem *reload = visibleNamed(window, QStringLiteral("tagEditorReloadButton"));
    QQuickItem *save = visibleNamed(window, QStringLiteral("tagEditorSaveButton"));
    QQuickItem *cancel = visibleNamed(window, QStringLiteral("tagEditorCancelButton"));
    QVERIFY(tools != nullptr && reload != nullptr && save != nullptr && cancel != nullptr);
    // Tools and Reload on the left; Save, then Cancel, on the right.
    QVERIFY(sceneRect(tools).right() < sceneRect(reload).left());
    QVERIFY(sceneRect(reload).right() < sceneRect(save).left());
    QVERIFY(sceneRect(save).right() < sceneRect(cancel).left());
    QVERIFY(evaluate(save, QStringLiteral("Accessible.defaultButton")).toBool());
    click(window, tools);
    QObject *menu = object(window, QStringLiteral("tagEditorToolsMenu"));
    QTRY_VERIFY(menu->property("opened").toBool());
    QVERIFY(emitClicked(object(window, QStringLiteral("tagEditorTitleCase_title"))));
    QTRY_COMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("The Long Way Home"));
    QTRY_VERIFY(visibleNamed(window, QStringLiteral("tagEditorField_title"))->property("text").toString()
                == QStringLiteral("The Long Way Home"));
    menu->setProperty("visible", false);
    editor.close();
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

void UiDialogsTest::dialogsPutTheirButtonsInKdesOrder() {
    TagEditorController editor(nullptr);
    editor.m_executeOperation = [](TagEditorController::Operation, const QByteArray &,
                                   const TagEditorController::RenameGate &) { return tagRows(2); };
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    // The tag editor's dialogs open over it.
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/Album")}));
    QTRY_VERIFY(!editor.loading() && object(window, QStringLiteral("tagEditorDialog"))->property("opened").toBool());
    editor.selectAllRows();
    editor.applyBulkField(QStringLiteral("title"), QStringLiteral("Edited"));
    // The affirmative button first, then the destructive one, then Cancel,
    // on the right, as KDE orders them.
    struct Order { const char *dialog; QStringList buttons; };
    const QList<Order> orders{
        {"quitSaveFailedDialog", {QStringLiteral("quitSaveFailedRetryButton"), QStringLiteral("quitSaveFailedQuitButton")}},
        {"tagEditorCloseConfirmDialog", {QStringLiteral("tagEditorCloseConfirmSaveButton"),
                                         QStringLiteral("tagEditorCloseConfirmDiscardButton"),
                                         QStringLiteral("tagEditorCloseConfirmKeepEditingButton")}},
        {"autoNumberDialog", {QStringLiteral("autoNumberApplyButton"), QStringLiteral("autoNumberCancelButton")}},
        {"renamePreviewDialog", {QStringLiteral("renamePreviewRenameButton"), QStringLiteral("renamePreviewCancelButton")}},
        {"removeLibraryRootConfirmDialog", {QStringLiteral("confirmDialogConfirmButton"),
                                            QStringLiteral("confirmDialogCancelButton")}},
    };
    // The confirmation of the library page opens over Configure Ferrous.
    QVERIFY(openPreferences(window, 0) != nullptr);
    QTRY_VERIFY(object(window, QStringLiteral("removeLibraryRootConfirmDialog")) != nullptr);
    for (const Order &order : orders) {
        QObject *dialog = object(window, QString::fromLatin1(order.dialog));
        QVERIFY2(dialog != nullptr, order.dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY2(dialog->property("opened").toBool(), order.dialog);
        qreal right = -1;
        for (const QString &name : order.buttons) {
            QQuickItem *button = nullptr;
            for (QQuickItem *candidate : itemsNamed(qvariant_cast<QQuickItem *>(dialog->property("footer")), name)) {
                button = candidate;
            }
            QVERIFY2(button != nullptr && button->isVisible(), qPrintable(name));
            QVERIFY2(sceneRect(button).left() > right, qPrintable(name));
            right = sceneRect(button).right();
        }
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
    }
}

void UiDialogsTest::dialogsUseOneCharacterEllipsesAndNoTextGlyphs() {
    TagEditorController editor(nullptr);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    for (const char *name : {"preferencesDialog", "aboutDialog", "tagEditorDialog", "recentErrorsDialog",
                             "quitSaveFailedDialog", "autoNumberDialog", "renamePreviewDialog",
                             "tagEditorCloseConfirmDialog", "tagEditorQuitWaitDialog", "libraryRootNameDialog"}) {
        QObject *dialog = object(window, QString::fromLatin1(name));
        QVERIFY2(dialog != nullptr, name);
        if (QString::fromLatin1(name) == QStringLiteral("preferencesDialog")) {
            QVERIFY(openPreferences(window) != nullptr);
            QTRY_VERIFY(visibleNamed(window, QStringLiteral("preferencesPages")) != nullptr);
        }
        const QStringList dots = textsHolding(dialog, QStringLiteral("..."));
        QVERIFY2(dots.isEmpty(), qPrintable(dots.join(QLatin1Char('\n'))));
        for (const QString &glyph : {QStringLiteral("\u00d7"), QStringLiteral("\u2715")}) {
            const QStringList glyphs = textsHolding(dialog, glyph);
            QVERIFY2(glyphs.isEmpty(), qPrintable(glyphs.join(QLatin1Char('\n'))));
        }
        QMetaObject::invokeMethod(dialog, "close");
    }
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h.
    isolateFromDesktopSessionWithAppLook();
    // Settings, data and caches of the test's own: no developer's colours,
    // fonts, layout or caches reach the tests.
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    QApplication app(argc, argv);
    UiDialogsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_dialogs.moc"
