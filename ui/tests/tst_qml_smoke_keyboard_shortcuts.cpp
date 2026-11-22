// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QStyle>
#include <QStyleFactory>

#include "../src/StandardKeys.h"

// Settings › Configure Keyboard Shortcuts (#458): the editor, its checks,
// and the shortcuts it applies to the window and the viewers. CTest runs
// these on a session bus that does not exist, so KeySequenceItem's checks
// against KDE's global shortcuts never reach the desktop's.
class KeyboardShortcutsSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void keptKeysAndTypedKeysAreTold();
    void listsTheActionsByMenuWithTheirKeys();
    void capturedKeysApplyToTheWindowAndTheViewers();
    void keysAnotherActionHasMoveOnlyWhenTheUserAgrees();
    void keysTheWindowKeepsAreRefused();
    void defaultsAndCancelLeaveOnlyWhatIsApplied();
    void searchFindsActionsByNameAndByKey();
    void shortcutsTheBridgeRefusesStayToChange();
    void captureWorksWithoutKdesGlobalShortcuts();
    void visualizationPageNamesResetZoomsKey();
    void editorHasOneBaselineAndOneKeyButtonWidth_data();
    void editorHasOneBaselineAndOneKeyButtonWidth();
};

namespace {

// The main window with its shortcuts dialog open: the window, the dialog,
// and the bridge the window has.
struct ShortcutsFixture {
    // KeySequenceItem grabs the keyboard as it captures, which the
    // offscreen platform cannot do, and says so.
    ~ShortcutsFixture() { dropCapturedMessages(QStringLiteral("This plugin does not support grabbing the keyboard")); }

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window{nullptr};
    QQuickWindow *dialog{nullptr};
    QObject *bridge{nullptr};

    // Loads the window and opens the dialog from its menu item, checking
    // no global or standard shortcuts unless `kdeChecks`.
    bool open(bool kdeChecks = false) {
        if (window == nullptr) {
            window = loadActiveMainWindow(engine, libraryModel);
            if (window == nullptr) {
                return false;
            }
            bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
            dialog = window->findChild<QQuickWindow *>(QStringLiteral("keyboardShortcutsDialog"));
            if (bridge == nullptr || dialog == nullptr) {
                return false;
            }
        }
        if (!kdeChecks) {
            dialog->setProperty("keyConflictChecks", 0);
        }
        QObject *action = window->findChild<QObject *>(QStringLiteral("configureShortcutsAction"));
        if (action == nullptr || !QMetaObject::invokeMethod(action, "trigger")) {
            return false;
        }
        return QTest::qWaitFor([this] { return dialog->isVisible(); }) && QTest::qWaitForWindowExposed(dialog);
    }

    QObject *action(const char *name) const {
        return window->findChild<QObject *>(QString::fromLatin1(name));
    }

    QQuickItem *item(const char *name) const {
        return dialog->findChild<QQuickItem *>(QString::fromLatin1(name));
    }

    QVariantMap stored() const {
        return bridge->property("keyboardShortcuts").toMap();
    }

    // The rows the list shows, by shortcutId.
    QStringList rows() const {
        QStringList ids;
        auto *model = qvariant_cast<QAbstractItemModel *>(item("keyboardShortcutsList")->property("model"));
        const int role = model->roleNames().key("shortcutId");
        for (int row = 0; row < model->rowCount(); ++row) {
            ids << model->index(row, 0).data(role).toString();
        }
        return ids;
    }

    // Makes the row of `shortcutId` current, as a click on it does.
    bool select(const QString &shortcutId) {
        QVariant shown;
        QMetaObject::invokeMethod(dialog, "selectAction", Q_RETURN_ARG(QVariant, shown),
                                  Q_ARG(QVariant, shortcutId));
        return shown.toBool() && dialog->property("currentShortcutId").toString() == shortcutId;
    }

    // What the dialog shows for `shortcutId`, changes waiting included.
    QStringList shownBindings(const QString &shortcutId) const {
        QVariant bindings;
        for (QObject *candidate : window->findChildren<QObject *>()) {
            if (candidate->property("shortcutId").toString() == shortcutId
                && candidate->property("keyBindings").isValid()) {
                QMetaObject::invokeMethod(dialog, "bindingsOf", Q_RETURN_ARG(QVariant, bindings),
                                          Q_ARG(QVariant, QVariant::fromValue(candidate)));
                break;
            }
        }
        if (bindings.metaType() == QMetaType::fromType<QJSValue>()) {
            bindings = bindings.value<QJSValue>().toVariant();
        }
        return bindings.toStringList();
    }

    // Presses `key` in the editor `editorName`, as a user who clicked it
    // would; whether the editor took it, and the dialog with it.
    bool capture(const char *editorName, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QQuickItem *editor = item(editorName);
        if (editor == nullptr || !QMetaObject::invokeMethod(editor, "startCapturing")) {
            return false;
        }
        QSignalSpy modified(editor, SIGNAL(keySequenceModified()));
        QTest::keyClick(dialog, key, modifiers);
        return modified.count() == 1 || modified.wait(2000);
    }

    QString editorText(const char *editorName) const {
        return StandardKeys::portableText(item(editorName)->property("keySequence"));
    }

    // Gives the dialog the size it opens at on a large screen with the
    // font at `pointSize`, 10 or 24, and lays it out; whether it took it.
    bool showAtSizeFor(int pointSize) const {
        const QSize size = pointSize > 10 ? QSize(1728, 1364) : QSize(720, 684);
        dialog->resize(size);
        return QTest::qWaitFor([&] { return dialog->size() == size; }) && !dialog->grabWindow().isNull();
    }

    bool click(const char *buttonName) const {
        return clickVisibleButton(dialog, QString::fromLatin1(buttonName));
    }
};

// The visible item named `objectName` in `root`'s item tree: a view's
// delegates have no QObject parent to find them by.
QQuickItem *visibleItemIn(QQuickItem *root, const QString &objectName) {
    for (QQuickItem *child : root->childItems()) {
        if (!child->isVisible()) {
            continue;
        }
        if (child->objectName() == objectName) {
            return child;
        }
        if (QQuickItem *found = visibleItemIn(child, objectName)) {
            return found;
        }
    }
    return nullptr;
}

// The widget style named, until the end of the scope; none keeps the test's.
class ScopedWidgetStyle {
public:
    explicit ScopedWidgetStyle(const QString &name)
        : m_previous(QApplication::style()->name()) {
        if (!name.isEmpty()) {
            QApplication::setStyle(name);
        }
    }
    ~ScopedWidgetStyle() { QApplication::setStyle(m_previous); }
    ScopedWidgetStyle(const ScopedWidgetStyle &) = delete;
    ScopedWidgetStyle &operator=(const ScopedWidgetStyle &) = delete;

private:
    QString m_previous;
};

// The test data of the editor's layout: both font sizes, in the tests'
// style and in Breeze.
void addFontAndStyleRows() {
    QTest::addColumn<int>("pointSize");
    QTest::addColumn<QString>("widgetStyle");
    for (const int pointSize : {10, 24}) {
        QTest::addRow("%d pt", pointSize) << pointSize << QString();
        QTest::addRow("%d pt, Breeze", pointSize) << pointSize << QStringLiteral("Breeze");
    }
}

// The item at or under `root` that says `text`, if one shows.
QQuickItem *visibleTextIn(QQuickItem *root, const QString &text) {
    for (QQuickItem *child : root->childItems()) {
        if (!child->isVisible()) {
            continue;
        }
        if (child->inherits("QQuickText") && child->property("text").toString() == text) {
            return child;
        }
        if (QQuickItem *found = visibleTextIn(child, text)) {
            return found;
        }
    }
    return nullptr;
}

// A KeySequenceItem's shown button: the checkable one, which takes the key,
// or the other, which clears it.
QQuickItem *keyButtonIn(QQuickItem *editor, bool takesKey) {
    for (QQuickItem *child : editor->childItems()) {
        const QVariant checkable = child->property("checkable");
        if (child->isVisible() && checkable.isValid() && checkable.toBool() == takesKey) {
            return child;
        }
    }
    return nullptr;
}

// The lowest row of `rect` with the ink of text, in `image`'s pixels: the
// baseline of text without descenders. Breeze draws a button's caption
// itself, so no item has its baseline. -1 for none.
int inkBottom(const QImage &image, const QRectF &rect) {
    const qreal dpr = image.devicePixelRatio();
    const QRect area = QRectF(rect.topLeft() * dpr, rect.size() * dpr).toAlignedRect().intersected(image.rect());
    const int ground = qGray(image.pixel(area.topLeft()));
    for (int y = area.bottom(); y >= area.top(); --y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (qAbs(qGray(image.pixel(x, y)) - ground) > 100) {
                return y;
            }
        }
    }
    return -1;
}

QStringList bindingsOf(QObject *action) {
    return action->property("keyBindings").toStringList();
}

QStringList portableBindings(QKeySequence::StandardKey key) {
    QStringList texts;
    for (const QKeySequence &binding : QKeySequence::keyBindings(key)) {
        texts << binding.toString(QKeySequence::PortableText);
    }
    return texts;
}

} // namespace

// The keys the editor refuses, by what keeps them, and those it allows
// with a note: one key each, its modifiers as the window takes them.
void KeyboardShortcutsSmokeTest::keptKeysAndTypedKeysAreTold() {
    const auto role = [](const char *text) { return StandardKeys::keptKeyRole(QString::fromLatin1(text)); };
    QCOMPARE(role("Return"), QStringLiteral("activate"));
    QCOMPARE(role("Enter"), QStringLiteral("activate"));
    QCOMPARE(StandardKeys::keptKeyRole(QKeySequence(Qt::KeypadModifier | Qt::Key_Enter)), QStringLiteral("activate"));
    QCOMPARE(role("Delete"), QStringLiteral("delete"));
    QCOMPARE(role("Backspace"), QStringLiteral("delete"));
    QCOMPARE(role("Tab"), QStringLiteral("focus"));
    QCOMPARE(role("Shift+Tab"), QStringLiteral("focus"));
    QCOMPARE(role("Backtab"), QStringLiteral("focus"));
    QCOMPARE(role("Shift+Backtab"), QStringLiteral("focus"));
    QCOMPARE(role("Esc"), QStringLiteral("cancel"));
    for (const char *key : {"Up", "Down", "Left", "Right", "PgUp", "PgDown", "Home", "End"}) {
        QCOMPARE(role(key), QStringLiteral("navigate"));
    }
    QCOMPARE(role("Menu"), QStringLiteral("contextMenu"));
    QCOMPARE(role("Shift+F10"), QStringLiteral("contextMenu"));
    QCOMPARE(role("Ctrl+M"), QStringLiteral("menuBar"));
    // With modifiers, the window leaves them to the shortcuts.
    for (const char *key : {"Ctrl+Return", "Shift+Delete", "Ctrl+Tab", "Shift+Esc", "Shift+Left", "Ctrl+Up",
                            "Alt+Home", "Ctrl+Shift+M", "F10", "M", "Space", "Ctrl+Alt+,"}) {
        QVERIFY2(role(key).isEmpty(), key);
    }
    QVERIFY(role("Ctrl+K, Delete").isEmpty());
    QVERIFY(role("").isEmpty());

    const auto types = [](const char *text) { return StandardKeys::typesCharacter(QString::fromLatin1(text)); };
    QVERIFY(types("F"));
    QVERIFY(types("Shift+F"));
    QVERIFY(types("7"));
    QVERIFY(types(","));
    QVERIFY(StandardKeys::typesCharacter(QKeySequence(Qt::Key_Adiaeresis)));
    for (const char *key : {"Space", "Ctrl+F", "Alt+F", "Meta+F", "F11", "Media Play", "Ctrl+Shift+Space", "Esc"}) {
        QVERIFY2(!types(key), key);
    }
}

// The dialog lists every action whose shortcuts can change, by menu, with
// its name as its menu item has it, less the mnemonic, and its keys; the
// menu item opens it.
void KeyboardShortcutsSmokeTest::listsTheActionsByMenuWithTheirKeys() {
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    QCOMPARE(fixture.dialog->title(), QStringLiteral("Configure Keyboard Shortcuts — Ferrous"));

    QStringList expected;
    for (const QVariant &group : fixture.window->property("shortcutActionGroups").toList()) {
        for (const QVariant &action : group.toMap().value(QStringLiteral("actions")).toList()) {
            expected << qvariant_cast<QObject *>(action)->property("shortcutId").toString();
        }
    }
    QCOMPARE(fixture.rows(), expected);
    QVERIFY(expected.contains(QStringLiteral("configure_shortcuts")));
    QCOMPARE(expected.indexOf(QStringLiteral("configure_shortcuts")) + 1, expected.indexOf(QStringLiteral("configure")));

    QQuickItem *list = fixture.item("keyboardShortcutsList");
    // The list makes the rows in view: each in turn is brought there.
    const auto cell = [&](const QString &id, const char *name) {
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fixture.rows().indexOf(id)),
                                  Q_ARG(int, 0));
        QQuickItem *row = visibleItemIn(list, QStringLiteral("keyboardShortcutsRow_") + id);
        return row != nullptr ? row->findChild<QQuickItem *>(QString::fromLatin1(name))->property("text").toString()
                              : QStringLiteral("<no row>");
    };
    QCOMPARE(cell(QStringLiteral("open_files"), "keyboardShortcutsRowName"), QStringLiteral("Open Files…"));
    QCOMPARE(cell(QStringLiteral("open_files"), "keyboardShortcutsRowShortcut"),
             QKeySequence::keyBindings(QKeySequence::Open).value(0).toString(QKeySequence::NativeText));
    QCOMPARE(cell(QStringLiteral("stop"), "keyboardShortcutsRowShortcut"), QStringLiteral("Ctrl+Shift+Space"));
    QCOMPARE(cell(QStringLiteral("stop"), "keyboardShortcutsRowAlternate"), QString());
    // Its own name, not what it does now.
    QCOMPARE(cell(QStringLiteral("play_pause"), "keyboardShortcutsRowName"), QStringLiteral("Play/Pause"));
    QCOMPARE(cell(QStringLiteral("undo"), "keyboardShortcutsRowName"), QStringLiteral("Undo"));
    // The section headings are the menus'.
    QStringList headings;
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    for (QQuickItem *section : list->property("contentItem").value<QQuickItem *>()->childItems()) {
        for (QQuickItem *heading : section->childItems()) {
            if (heading->isVisible() && heading->objectName() == QStringLiteral("keyboardShortcutsGroupHeading")) {
                headings << heading->property("text").toString();
            }
        }
    }
    QVERIFY2(headings.contains(QStringLiteral("File")), qPrintable(headings.join(QLatin1Char(','))));

    // The first row is current as it opens, and the keyboard is in the
    // search field.
    QCOMPARE(fixture.dialog->property("currentShortcutId").toString(), expected.first());
    QVERIFY(fixture.item("keyboardShortcutsSearchField")->hasActiveFocus());
    // The editor shows the current row's keys and its defaults.
    QVERIFY(fixture.select(QStringLiteral("stop")));
    QCOMPARE(fixture.editorText("keyboardShortcutsShortcutEditor"), QStringLiteral("Ctrl+Shift+Space"));
    QCOMPARE(fixture.editorText("keyboardShortcutsAlternateEditor"), QString());
    QVERIFY(fixture.item("keyboardShortcutsDefaultRadio")->property("checked").toBool());
    QVERIFY(!fixture.item("keyboardShortcutsCustomRadio")->property("checked").toBool());
    QCOMPARE(fixture.item("keyboardShortcutsDefaultRadio")->property("text").toString(),
             QStringLiteral("Default: Ctrl+Shift+Space"));
    QVERIFY(fixture.select(QStringLiteral("add_files")));
    QCOMPARE(fixture.item("keyboardShortcutsDefaultRadio")->property("text").toString(), QStringLiteral("Default: None"));

    QTest::keyClick(fixture.dialog, Qt::Key_Escape);
    QTRY_VERIFY(!fixture.dialog->isVisible());
}

// A key pressed in the editor waits in the dialog until Apply or OK, which
// send only the shortcuts that differ from the defaults; they then work
// at once: the shortcut, the alternate in the window, and both in a
// viewer, which repeats the window's.
void KeyboardShortcutsSmokeTest::capturedKeysApplyToTheWindowAndTheViewers() {
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    QObject *stop = fixture.action("stopAction");
    QVERIFY(stop != nullptr);
    QSignalSpy stopped(stop, SIGNAL(triggered(QObject*)));

    QVERIFY(fixture.select(QStringLiteral("stop")));
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_P, Qt::ControlModifier | Qt::AltModifier));
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Ctrl+Alt+P")});
    QCOMPARE(fixture.editorText("keyboardShortcutsShortcutEditor"), QStringLiteral("Ctrl+Alt+P"));
    QVERIFY(fixture.item("keyboardShortcutsCustomRadio")->property("checked").toBool());
    QVERIFY(!fixture.item("keyboardShortcutsDefaultRadio")->property("checked").toBool());
    // Not yet applied.
    QCOMPARE(bindingsOf(stop), QStringList{QStringLiteral("Ctrl+Shift+Space")});
    QVERIFY(fixture.stored().isEmpty());
    QVERIFY(fixture.dialog->property("modified").toBool());

    QVERIFY(fixture.click("keyboardShortcutsApplyButton"));
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("stop"), QStringLiteral("Ctrl+Alt+P")}}));
    QCOMPARE(stop->property("shortcut").value<QKeySequence>(), QKeySequence(QStringLiteral("Ctrl+Alt+P")));
    QVERIFY(!fixture.dialog->property("modified").toBool());
    QVERIFY(fixture.dialog->isVisible());

    QVERIFY(fixture.capture("keyboardShortcutsAlternateEditor", Qt::Key_J, Qt::ControlModifier | Qt::AltModifier));
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")),
             (QStringList{QStringLiteral("Ctrl+Alt+P"), QStringLiteral("Ctrl+Alt+J")}));
    QVERIFY(fixture.click("keyboardShortcutsOkButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("stop"), QStringLiteral("Ctrl+Alt+P; Ctrl+Alt+J")}}));

    // In the main window, the shortcut and the alternate; the default no
    // longer.
    QTRY_VERIFY(fixture.window->isActive());
    fixture.window->contentItem()->forceActiveFocus();
    QTest::keyClick(fixture.window, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(stopped.count(), 1);
    QTest::keyClick(fixture.window, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(stopped.count(), 2);
    QTest::keyClick(fixture.window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(stopped.count(), 2);

    // In the viewer, a modal popup that blocks the window's shortcuts.
    QObject *viewer = fixture.window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(viewer != nullptr);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = fixture.window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QTest::keyClick(fixture.window, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(stopped.count(), 3);
    QTest::keyClick(fixture.window, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(stopped.count(), 4);
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());

    // Cleared, the action has no shortcut, which the settings keep as such.
    QVERIFY(fixture.open());
    QVERIFY(fixture.select(QStringLiteral("stop")));
    QCOMPARE(fixture.editorText("keyboardShortcutsShortcutEditor"), QStringLiteral("Ctrl+Alt+P"));
    QCOMPARE(fixture.editorText("keyboardShortcutsAlternateEditor"), QStringLiteral("Ctrl+Alt+J"));
    fixture.item("keyboardShortcutsAlternateEditor")->setProperty("keySequence", QString());
    QVERIFY(QMetaObject::invokeMethod(fixture.item("keyboardShortcutsAlternateEditor"), "keySequenceModified"));
    fixture.item("keyboardShortcutsShortcutEditor")->setProperty("keySequence", QString());
    QVERIFY(QMetaObject::invokeMethod(fixture.item("keyboardShortcutsShortcutEditor"), "keySequenceModified"));
    QVERIFY(fixture.shownBindings(QStringLiteral("stop")).isEmpty());
    QVERIFY(fixture.click("keyboardShortcutsOkButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("stop"), QString()}}));
    QVERIFY(bindingsOf(stop).isEmpty());
}

// A key another action has asks first, as KDE's editor does: Cancel keeps
// both as they were, Reassign moves it.
void KeyboardShortcutsSmokeTest::keysAnotherActionHasMoveOnlyWhenTheUserAgrees() {
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    auto *reassign = fixture.dialog->findChild<QQuickWindow *>(QStringLiteral("keyboardShortcutsReassignDialog"));
    QVERIFY(reassign != nullptr);

    // Play/Pause's Space, for Stop.
    QVERIFY(fixture.select(QStringLiteral("stop")));
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_Space));
    QTRY_VERIFY(reassign->isVisible());
    QCOMPARE(reassign->title(), QStringLiteral("Shortcut Conflict"));
    QCOMPARE(reassign->property("message").toString(), QStringLiteral("Reassign ‘Space’ to ‘Stop’?"));
    QCOMPARE(reassign->property("details").toString(),
             QStringLiteral("The ‘Space’ key combination is already assigned to the ‘Play/Pause’ action, "
                            "which will no longer have it."));
    // Until the user agrees, the editor shows the keys as they were.
    QCOMPARE(fixture.editorText("keyboardShortcutsShortcutEditor"), QStringLiteral("Ctrl+Shift+Space"));
    QVERIFY(clickVisibleButton(reassign, QStringLiteral("confirmDialogCancelButton")));
    QTRY_VERIFY(!reassign->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Ctrl+Shift+Space")});
    QCOMPARE(fixture.shownBindings(QStringLiteral("play_pause")), QStringList{QStringLiteral("Space")});
    QVERIFY(!fixture.dialog->property("modified").toBool());

    // Reassigned, it moves; as the alternate, the shortcut stays.
    QVERIFY(fixture.capture("keyboardShortcutsAlternateEditor", Qt::Key_Space));
    QTRY_VERIFY(reassign->isVisible());
    QVERIFY(clickVisibleButton(reassign, QStringLiteral("confirmDialogConfirmButton")));
    QTRY_VERIFY(!reassign->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")),
             (QStringList{QStringLiteral("Ctrl+Shift+Space"), QStringLiteral("Space")}));
    QVERIFY(fixture.shownBindings(QStringLiteral("play_pause")).isEmpty());
    QCOMPARE(fixture.editorText("keyboardShortcutsAlternateEditor"), QStringLiteral("Space"));

    // A key the same action has already moves within it without asking.
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_Space));
    QVERIFY(!reassign->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Space")});

    // Play/Pause's Default would take Space back: that asks too.
    QVERIFY(fixture.select(QStringLiteral("play_pause")));
    QVERIFY(clickVisibleButton(fixture.dialog, QStringLiteral("keyboardShortcutsDefaultRadio")));
    QTRY_VERIFY(reassign->isVisible());
    QCOMPARE(reassign->property("message").toString(), QStringLiteral("Reassign ‘Space’ to ‘Play/Pause’?"));
    QCOMPARE(reassign->property("details").toString(),
             QStringLiteral("The ‘Space’ key combination is already assigned to the ‘Stop’ action, "
                            "which will no longer have it."));
    QVERIFY(clickVisibleButton(reassign, QStringLiteral("confirmDialogCancelButton")));
    QTRY_VERIFY(!reassign->isVisible());
    QVERIFY(fixture.shownBindings(QStringLiteral("play_pause")).isEmpty());
    QVERIFY(!fixture.item("keyboardShortcutsDefaultRadio")->property("checked").toBool());
    QVERIFY(clickVisibleButton(fixture.dialog, QStringLiteral("keyboardShortcutsDefaultRadio")));
    QTRY_VERIFY(reassign->isVisible());
    QVERIFY(clickVisibleButton(reassign, QStringLiteral("confirmDialogConfirmButton")));
    QTRY_VERIFY(!reassign->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("play_pause")), QStringList{QStringLiteral("Space")});
    QVERIFY(fixture.shownBindings(QStringLiteral("stop")).isEmpty());
    // Back to Stop's Space for what follows.
    QVERIFY(fixture.select(QStringLiteral("stop")));
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_Space));
    QTRY_VERIFY(reassign->isVisible());
    QVERIFY(clickVisibleButton(reassign, QStringLiteral("confirmDialogConfirmButton")));
    QTRY_VERIFY(!reassign->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Space")});

    QVERIFY(fixture.click("keyboardShortcutsOkButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("stop"), QStringLiteral("Space")},
                                            {QStringLiteral("play_pause"), QString()}}));
    QCOMPARE(bindingsOf(fixture.action("stopAction")), QStringList{QStringLiteral("Space")});
    QVERIFY(bindingsOf(fixture.action("playPauseAction")).isEmpty());
}

// The keys the window, its lists and its text fields keep are refused,
// saying why; a key the library and text fields type is taken, with a
// note that they take it while they have the focus.
void KeyboardShortcutsSmokeTest::keysTheWindowKeepsAreRefused() {
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    QQuickItem *refusal = fixture.item("keyboardShortcutsRefusal");
    QQuickItem *note = fixture.item("keyboardShortcutsTypedKeyNote");
    QVERIFY(refusal != nullptr && note != nullptr);
    QVERIFY(fixture.select(QStringLiteral("stop")));
    QVERIFY(!refusal->isVisible());

    struct Kept {
        Qt::Key key;
        Qt::KeyboardModifiers modifiers;
        const char *says;
    };
    for (const Kept &kept : {Kept{Qt::Key_Delete, Qt::NoModifier, "removes the selected tracks"},
                             Kept{Qt::Key_Home, Qt::NoModifier, "moves the selection"},
                             Kept{Qt::Key_M, Qt::ControlModifier, "menu bar"},
                             Kept{Qt::Key_F10, Qt::ShiftModifier, "context menu"},
                             Kept{Qt::Key_Return, Qt::NoModifier, "default button"}}) {
        QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", kept.key, kept.modifiers));
        QVERIFY2(refusal->isVisible(), kept.says);
        QVERIFY2(refusal->property("text").toString().contains(QString::fromLatin1(kept.says)),
                 qPrintable(refusal->property("text").toString()));
        QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Ctrl+Shift+Space")});
        QCOMPARE(fixture.editorText("keyboardShortcutsShortcutEditor"), QStringLiteral("Ctrl+Shift+Space"));
        QVERIFY(!fixture.dialog->property("modified").toBool());
    }
    // Escape, as the key captured, is refused too rather than closing the
    // dialog.
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_Escape));
    QVERIFY(fixture.dialog->isVisible());
    QVERIFY(refusal->property("text").toString().contains(QStringLiteral("closes")));

    // A plain letter is taken, with the note.
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_G));
    QVERIFY(!refusal->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("G")});
    QVERIFY(note->isVisible());
    QCOMPARE(note->property("text").toString(),
             QStringLiteral("While the library or a text field has the keyboard focus, ‘G’ types there instead."));

    // The album art viewer's I, for an action the viewers repeat; a key
    // any other action may take.
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_I));
    QVERIFY(refusal->isVisible());
    QVERIFY(refusal->property("text").toString().contains(QStringLiteral("album art viewer")));
    QVERIFY(fixture.select(QStringLiteral("clear_queue")));
    QVERIFY(!refusal->isVisible());
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_I));
    QVERIFY(!refusal->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("clear_queue")), QStringList{QStringLiteral("I")});

    QTest::keyClick(fixture.dialog, Qt::Key_Escape);
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QVERIFY(fixture.stored().isEmpty());
}

// Defaults sets every action back to its defaults, a standard key's
// following the platform's, to apply in turn; Cancel drops what was not
// applied. Shortcuts of actions this build does not have stay.
void KeyboardShortcutsSmokeTest::defaultsAndCancelLeaveOnlyWhatIsApplied() {
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    QTest::keyClick(fixture.dialog, Qt::Key_Escape);
    QTRY_VERIFY(!fixture.dialog->isVisible());
    // "constructor" names a property of every JavaScript object.
    const QVariantMap changed{{QStringLiteral("stop"), QString()},
                              {QStringLiteral("open_files"), QStringLiteral("Ctrl+Alt+O")},
                              {QStringLiteral("future_action"), QStringLiteral("Ctrl+Alt+K")},
                              {QStringLiteral("constructor"), QStringLiteral("Ctrl+Alt+C")}};
    QVERIFY(QMetaObject::invokeMethod(fixture.bridge, "setKeyboardShortcuts", Q_ARG(QVariant, changed)));
    QCOMPARE(fixture.stored(), changed);

    // Cancel, after Defaults: nothing changes.
    QVERIFY(fixture.open());
    QVERIFY(fixture.item("keyboardShortcutsDefaultsButton")->isEnabled());
    QVERIFY(fixture.select(QStringLiteral("open_files")));
    QVERIFY(fixture.item("keyboardShortcutsCustomRadio")->property("checked").toBool());
    QVERIFY(fixture.click("keyboardShortcutsDefaultsButton"));
    QCOMPARE(fixture.shownBindings(QStringLiteral("open_files")), portableBindings(QKeySequence::Open));
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Ctrl+Shift+Space")});
    QVERIFY(fixture.item("keyboardShortcutsDefaultRadio")->property("checked").toBool());
    QVERIFY(!fixture.item("keyboardShortcutsDefaultsButton")->isEnabled());
    QVERIFY(fixture.click("keyboardShortcutsCancelButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), changed);
    QCOMPARE(bindingsOf(fixture.action("openFilesAction")), QStringList{QStringLiteral("Ctrl+Alt+O")});

    // Opened again, it shows what is applied, not what was cancelled.
    QVERIFY(fixture.open());
    QCOMPARE(fixture.shownBindings(QStringLiteral("open_files")), QStringList{QStringLiteral("Ctrl+Alt+O")});
    QVERIFY(!fixture.dialog->property("modified").toBool());

    // One action's Default, then OK: only that one changes.
    QVERIFY(fixture.select(QStringLiteral("open_files")));
    QVERIFY(clickVisibleButton(fixture.dialog, QStringLiteral("keyboardShortcutsDefaultRadio")));
    QCOMPARE(fixture.shownBindings(QStringLiteral("open_files")), portableBindings(QKeySequence::Open));
    QVERIFY(fixture.item("keyboardShortcutsDefaultRadio")->property("checked").toBool());
    QVERIFY(fixture.click("keyboardShortcutsOkButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("stop"), QString()},
                                            {QStringLiteral("future_action"), QStringLiteral("Ctrl+Alt+K")},
                                            {QStringLiteral("constructor"), QStringLiteral("Ctrl+Alt+C")}}));
    QCOMPARE(bindingsOf(fixture.action("openFilesAction")), portableBindings(QKeySequence::Open));

    // Defaults and OK: none but the unknown action's.
    QVERIFY(fixture.open());
    QVERIFY(fixture.click("keyboardShortcutsDefaultsButton"));
    QVERIFY(fixture.click("keyboardShortcutsOkButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("future_action"), QStringLiteral("Ctrl+Alt+K")},
                                            {QStringLiteral("constructor"), QStringLiteral("Ctrl+Alt+C")}}));
    QCOMPARE(bindingsOf(fixture.action("stopAction")), QStringList{QStringLiteral("Ctrl+Shift+Space")});

    // Custom on an action at its defaults starts the capture; the key
    // pressed makes it the action's own.
    QVERIFY(fixture.open());
    QVERIFY(fixture.select(QStringLiteral("add_files")));
    QVERIFY(clickVisibleButton(fixture.dialog, QStringLiteral("keyboardShortcutsCustomRadio")));
    QVERIFY(fixture.item("keyboardShortcutsCustomRadio")->property("checked").toBool());
    QVERIFY(!fixture.item("keyboardShortcutsDefaultRadio")->property("checked").toBool());
    QSignalSpy modified(fixture.item("keyboardShortcutsShortcutEditor"), SIGNAL(keySequenceModified()));
    QTest::keyClick(fixture.dialog, Qt::Key_A, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(modified.count(), 1);
    QCOMPARE(fixture.shownBindings(QStringLiteral("add_files")), QStringList{QStringLiteral("Ctrl+Alt+A")});
    QVERIFY(fixture.click("keyboardShortcutsCancelButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("future_action"), QStringLiteral("Ctrl+Alt+K")},
                                            {QStringLiteral("constructor"), QStringLiteral("Ctrl+Alt+C")}}));
}

// The search finds an action by its name, its mnemonic left out, or by
// its keys; the current row stays current while it shows.
void KeyboardShortcutsSmokeTest::searchFindsActionsByNameAndByKey() {
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    QQuickItem *field = fixture.item("keyboardShortcutsSearchField");
    QVERIFY(field != nullptr && field->hasActiveFocus());
    const int all = fixture.rows().size();

    for (const char letter : {'z', 'o', 'o', 'm'}) {
        QTest::keyClick(fixture.dialog, letter);
    }
    QCOMPARE(fixture.rows(), QStringList{QStringLiteral("reset_zoom")});
    QCOMPARE(fixture.dialog->property("currentShortcutId").toString(), QStringLiteral("reset_zoom"));

    // "Configure Keyboard S&hortcuts…".
    field->setProperty("text", QStringLiteral("shortcuts"));
    QCOMPARE(fixture.rows(), QStringList{QStringLiteral("configure_shortcuts")});

    // By its key.
    field->setProperty("text", QStringLiteral("ctrl+shift+space"));
    QCOMPARE(fixture.rows(), QStringList{QStringLiteral("stop")});
    // An edit keeps the row it changed, though its new key no longer
    // matches; the next search follows the new key.
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_P, Qt::ControlModifier | Qt::AltModifier));
    QCOMPARE(fixture.rows(), QStringList{QStringLiteral("stop")});
    QCOMPARE(fixture.dialog->property("currentShortcutId").toString(), QStringLiteral("stop"));
    field->setProperty("text", QStringLiteral("ctrl+shift+spac"));
    QVERIFY(fixture.rows().isEmpty());
    field->setProperty("text", QStringLiteral("ctrl+alt+p"));
    QCOMPARE(fixture.rows(), QStringList{QStringLiteral("stop")});
    field->setProperty("text", QStringLiteral("F11"));
    QCOMPARE(fixture.rows(), (QStringList{QStringLiteral("visualization_viewer"), QStringLiteral("album_art_viewer")}));

    // The current row stays while it shows.
    QVERIFY(fixture.select(QStringLiteral("album_art_viewer")));
    field->setProperty("text", QStringLiteral("viewer"));
    QVERIFY(fixture.rows().contains(QStringLiteral("album_art_viewer")));
    QCOMPARE(fixture.dialog->property("currentShortcutId").toString(), QStringLiteral("album_art_viewer"));

    // No match: no row, and no action for the editor.
    field->setProperty("text", QStringLiteral("no such action"));
    QVERIFY(fixture.rows().isEmpty());
    QVERIFY(fixture.item("keyboardShortcutsNoMatches")->isVisible());
    QVERIFY(!fixture.item("keyboardShortcutsEditor")->isEnabled());

    field->setProperty("text", QString());
    QCOMPARE(fixture.rows().size(), all);
    // The down key moves through the list from the field.
    const QString first = fixture.dialog->property("currentShortcutId").toString();
    field->forceActiveFocus();
    QTest::keyClick(fixture.dialog, Qt::Key_Down);
    QCOMPARE(fixture.dialog->property("currentShortcutId").toString(), fixture.rows().value(1));
    QVERIFY(first != fixture.rows().value(1));
    QTest::keyClick(fixture.dialog, Qt::Key_Escape);
    QTRY_VERIFY(!fixture.dialog->isVisible());
}

// Shortcuts the bridge does not take, as out of its bounds, stay in the
// dialog, which says so and stays open, until changed or cancelled.
void KeyboardShortcutsSmokeTest::shortcutsTheBridgeRefusesStayToChange() {
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    QQmlComponent component(&fixture.engine);
    component.setData(QByteArrayLiteral(R"QML(
import QtQml 2.15
QtObject {
    property var keyboardShortcuts: ({})
    property int requests: 0
    function setKeyboardShortcuts(shortcuts) { requests++; return false }
}
)QML"), QUrl());
    std::unique_ptr<QObject> refusing(component.create());
    QVERIFY2(refusing != nullptr, qPrintable(component.errorString()));
    fixture.dialog->setProperty("uiBridge", QVariant::fromValue(refusing.get()));
    QQuickItem *failed = fixture.item("keyboardShortcutsSaveFailed");
    QVERIFY(failed != nullptr && !failed->isVisible());

    QVERIFY(fixture.select(QStringLiteral("stop")));
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_P, Qt::ControlModifier | Qt::AltModifier));
    QVERIFY(fixture.click("keyboardShortcutsOkButton"));
    QCOMPARE(refusing->property("requests").toInt(), 1);
    QVERIFY(fixture.dialog->isVisible());
    QVERIFY(failed->isVisible());
    QVERIFY(fixture.dialog->property("modified").toBool());
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Ctrl+Alt+P")});

    // Cancelled, the dialog drops them, and the next opening starts clean.
    QVERIFY(fixture.click("keyboardShortcutsCancelButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    fixture.dialog->setProperty("uiBridge", QVariant::fromValue(fixture.bridge));
    QVERIFY(fixture.open());
    QVERIFY(!fixture.item("keyboardShortcutsSaveFailed")->isVisible());
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Ctrl+Shift+Space")});
    QTest::keyClick(fixture.dialog, Qt::Key_Escape);
    QTRY_VERIFY(!fixture.dialog->isVisible());
}

// With its checks against KDE's global and standard shortcuts on, as the
// app has them, the editor still takes a key where the session has no
// kglobalaccel to ask.
void KeyboardShortcutsSmokeTest::captureWorksWithoutKdesGlobalShortcuts() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && bus.interface() != nullptr
        && bus.interface()->isServiceRegistered(QStringLiteral("org.kde.kglobalaccel"))) {
        QSKIP("The session bus has kglobalaccel; CTest runs this test without a session bus.");
    }
    ShortcutsFixture fixture;
    QVERIFY(fixture.open(true));
    QVERIFY(fixture.dialog->property("keyConflictChecks").toInt() != 0);
    QVERIFY(fixture.select(QStringLiteral("stop")));
    QVERIFY(fixture.capture("keyboardShortcutsShortcutEditor", Qt::Key_P, Qt::ControlModifier | Qt::AltModifier));
    QCOMPARE(fixture.shownBindings(QStringLiteral("stop")), QStringList{QStringLiteral("Ctrl+Alt+P")});
    QVERIFY(fixture.click("keyboardShortcutsOkButton"));
    QTRY_VERIFY(!fixture.dialog->isVisible());
    QCOMPARE(fixture.stored(), (QVariantMap{{QStringLiteral("stop"), QStringLiteral("Ctrl+Alt+P")}}));
    // KGlobalAccel's word that it found no kglobalaccel to ask.
    dropCapturedMessagesMatching(QRegularExpression(QStringLiteral("^Couldn't start kglobalaccel")));
}

// The Visualization page names Reset Zoom's key as it is now, and no key
// where it has none.
void KeyboardShortcutsSmokeTest::visualizationPageNamesResetZoomsKey() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *preferences = window->findChild<QObject *>(QStringLiteral("preferencesDialog"));
    QVERIFY(bridge != nullptr && preferences != nullptr);
    QVERIFY(QMetaObject::invokeMethod(preferences, "openPage", Q_ARG(QVariant, 1)));
    QTRY_VERIFY(preferences->property("visible").toBool());
    const auto helpText = [&] {
        QQuickItem *help = preferences->findChild<QQuickItem *>(QStringLiteral("zoomHelp"));
        return help != nullptr ? help->property("text").toString() : QString();
    };
    QTRY_VERIFY(helpText().endsWith(QStringLiteral("Ctrl+0 resets the zoom.")));
    const QVariant changed = QVariantMap{{QStringLiteral("reset_zoom"), QStringLiteral("Ctrl+Alt+0")}};
    QVERIFY(QMetaObject::invokeMethod(bridge, "setKeyboardShortcuts", Q_ARG(QVariant, changed)));
    QVERIFY(helpText().endsWith(QStringLiteral("Ctrl+Alt+0 resets the zoom.")));
    const QVariant cleared = QVariantMap{{QStringLiteral("reset_zoom"), QString()}};
    QVERIFY(QMetaObject::invokeMethod(bridge, "setKeyboardShortcuts", Q_ARG(QVariant, cleared)));
    QVERIFY(!helpText().contains(QStringLiteral("resets the zoom")));
    QVERIFY(helpText().endsWith(QStringLiteral("centered spectrogram.")));
    QVERIFY(QMetaObject::invokeMethod(preferences, "close"));
    QTRY_VERIFY(!preferences->property("visible").toBool());
}

void KeyboardShortcutsSmokeTest::editorHasOneBaselineAndOneKeyButtonWidth_data() {
    addFontAndStyleRows();
}

// Custom:, Shortcut: and the key on its button share a baseline (Custom:
// was 5 px above the two beside it). The two key buttons are one width,
// whatever keys they show, so the buttons that clear them share an x.
void KeyboardShortcutsSmokeTest::editorHasOneBaselineAndOneKeyButtonWidth() {
    QFETCH(int, pointSize);
    QFETCH(QString, widgetStyle);
    // Breeze's buttons are taller than its radio buttons, which set the
    // radio's caption off the button's; Fusion's are as tall.
    if (!widgetStyle.isEmpty() && !QStyleFactory::keys().contains(widgetStyle, Qt::CaseInsensitive)) {
        QSKIP("The Breeze widget style is not installed.");
    }
    const ScopedWidgetStyle style(widgetStyle);
    const ScopedApplicationFont font(pointSize);
    ShortcutsFixture fixture;
    QVERIFY(fixture.open());
    QVERIFY(fixture.select(QStringLiteral("open_files")));
    QVERIFY(fixture.showAtSizeFor(pointSize));
    QQuickItem *editor = fixture.item("keyboardShortcutsEditor");
    QQuickItem *shortcut = fixture.item("keyboardShortcutsShortcutEditor");
    QQuickItem *alternate = fixture.item("keyboardShortcutsAlternateEditor");
    auto *customText = qvariant_cast<QQuickItem *>(
        fixture.item("keyboardShortcutsCustomRadio")->property("contentItem"));
    QQuickItem *label = visibleTextIn(editor, QStringLiteral("Shortcut:"));
    QQuickItem *key = keyButtonIn(shortcut, true);
    QVERIFY(customText != nullptr && label != nullptr && key != nullptr);

    const QImage shown = fixture.dialog->grabWindow();
    // Right of the radio's indicator, and of the key button's glyph.
    const int custom = inkBottom(shown, sceneRect(customText).adjusted(customText->property("leftPadding").toReal(), 0, 0, 0));
    const int named = inkBottom(shown, sceneRect(label));
    const int drawn = inkBottom(shown, sceneRect(key).adjusted(key->width() * 0.4, 5, -6, -5));
    QVERIFY(custom >= 0 && named >= 0 && drawn >= 0);
    const QString baselines = QStringLiteral("Custom: %1, Shortcut: %2, key %3").arg(custom).arg(named).arg(drawn);
    // Breeze rounds the caption it draws to pixels of its own.
    QVERIFY2(custom == named && qAbs(drawn - named) <= 1, qPrintable(baselines));

    const qreal editorX = sceneRect(shortcut).left();
    for (const char *action : {"open_files", "stop"}) {
        QVERIFY(fixture.select(QString::fromLatin1(action)));
        QVERIFY(!fixture.dialog->grabWindow().isNull());
        QQuickItem *first = keyButtonIn(shortcut, true);
        QQuickItem *second = keyButtonIn(alternate, true);
        QQuickItem *clearFirst = keyButtonIn(shortcut, false);
        QQuickItem *clearSecond = keyButtonIn(alternate, false);
        QVERIFY(first != nullptr && second != nullptr && clearFirst != nullptr && clearSecond != nullptr);
        QCOMPARE(first->width(), second->width());
        QVERIFY(first->width() >= std::max(first->implicitWidth(), second->implicitWidth()) - 0.5);
        QCOMPARE(sceneRect(clearFirst).left(), sceneRect(clearSecond).left());
        // The grid's columns follow its own cells, not the longer default.
        QCOMPARE(sceneRect(shortcut).left(), editorX);
    }
}

std::unique_ptr<QmlSmokeTest> makeKeyboardShortcutsSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<KeyboardShortcutsSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_keyboard_shortcuts.moc"
