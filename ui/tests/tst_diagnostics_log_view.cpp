// SPDX-License-Identifier: GPL-3.0-or-later

// The Diagnostics window's log view: what a full log costs it, and that it
// selects, copies and reads out as the text edit it replaced.

#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextLayout>
#include <QWheelEvent>
#include <QtTest/QtTest>

#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "../src/Localization.h"
#include "../src/QmlTypes.h"
// The tests look at the rows the view has ready for each frame.
#define private public
#include "../src/DiagnosticsLogView.h"
#undef private
#include "TestMessages.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

// Diagnostics' limit.
constexpr int kFullLog = 2000;

QString record(int index, int characters) {
    QString line = QStringLiteral("[2026-01-02T03:04:%1.000] [playback] info: Track started (user): \"/fixture/library/")
                       .arg(index % 60, 2, 10, QLatin1Char('0'));
    int part = 0;
    while (line.size() < characters) {
        line += QStringLiteral("An Artist/An album %1/%2 ").arg(index).arg(++part);
    }
    line.truncate(characters - 1);
    return line + QLatin1Char('"');
}

QStringList records(int count, int characters) {
    QStringList lines;
    for (int index = 0; index < count; ++index) {
        lines << record(index, characters);
    }
    return lines;
}

QString fullLog() {
    return records(kFullLog, 1000).join(QLatin1Char('\n'));
}

// Every kind of character that a text edit treats on its own.
QString trickyLog() {
    return QString::fromUtf16(
        u"  [a] <b>not bold</b> &amp; <img src=\"/fixture/x.png\">\n"
        u"[b] tab\there, no break, café café\r\n"
        u"[c] lone\rreturn, line separator, paragraph separator\n"
        u"\n"
        u"[d] 静かな子午線 \U0001f3b5 موسيقى frame﷐begin﷑end\n"
        u"[e] trailing spaces   \n");
}

QQuickItem *itemNamed(QQuickItem *root, const QString &name) {
    if (root->objectName() == name) {
        return root;
    }
    const QList<QQuickItem *> children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = itemNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

// The dialog over a window of its own, with a bridge that the test fills.
struct Diagnostics {
    Diagnostics() {
        QQmlComponent component(&engine);
        component.setData(R"QML(
import QtQuick
import QtQuick.Controls
import "components" as Components
import "dialogs" as Dialogs
ApplicationWindow {
    id: window
    width: 1200
    height: 800
    visible: true
    property alias dialog: dialog
    property alias bridge: bridge
    Components.UiPalette { id: uiPalette; windowRoot: window }
    QtObject {
        id: bridge
        property string diagnosticsText: ""
        property bool diagnosticsLoading: false
        property string diagnosticsLogPath: "/fixture/state/diagnostics.log"
        property int copiesWithoutPaths: 0
        signal diagnosticsChanged()
        function reloadDiagnosticsFromDisk() {}
        function clearDiagnostics() {}
        function openContainingFolder(path) {}
        function copyDiagnosticsWithoutPaths() { copiesWithoutPaths += 1 }
    }
    Dialogs.DiagnosticsDialog {
        id: dialog
        uiBridge: bridge
        uiPalette: uiPalette
        windowRoot: window
    }
}
)QML",
                          QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR)
                                              + QStringLiteral("/qml/DiagnosticsHarness.qml")));
        root.reset(component.create());
        if (root == nullptr) {
            qWarning("%s", qPrintable(component.errorString()));
            return;
        }
        bridge = root->property("bridge").value<QObject *>();
        window = qobject_cast<QQuickWindow *>(root->property("dialog").value<QObject *>());
        if (window == nullptr) {
            return;
        }
        view = root->findChild<DiagnosticsLogView *>();
        if (QQuickItem *scroll = itemNamed(window->contentItem(), QStringLiteral("diagnosticsScrollView"))) {
            flickable = scroll->property("contentItem").value<QQuickItem *>();
        }
    }

    bool loaded() const { return bridge != nullptr && view != nullptr && flickable != nullptr; }

    void setLog(const QString &text) const {
        bridge->setProperty("diagnosticsText", text);
        QMetaObject::invokeMethod(bridge, "diagnosticsChanged");
    }

    bool open() const {
        return QMetaObject::invokeMethod(window, "open") && QTest::qWaitForWindowExposed(window)
            && QTest::qWaitForWindowActive(window) && frame();
    }

    void close() const { QMetaObject::invokeMethod(window, "reject"); }

    // Waits for a frame that shows what changed.
    bool frame() const {
        QSignalSpy swapped(window, &QQuickWindow::frameSwapped);
        window->update();
        return swapped.wait(5000);
    }

    qreal contentY() const { return flickable->property("contentY").toReal(); }
    void scrollTo(qreal y) const { flickable->setProperty("contentY", y); }

    int rowsInView() const {
        return static_cast<int>(std::ceil(view->height() / view->m_layout.rowHeight())) + 1;
    }

    QPoint inWindow(int position) const {
        const QRectF at = view->positionToRectangle(position);
        return view->mapToScene(at.center()).toPoint();
    }

    QQmlApplicationEngine engine;
    std::unique_ptr<QObject> root;
    QObject *bridge = nullptr;
    QQuickWindow *window = nullptr;
    DiagnosticsLogView *view = nullptr;
    QQuickItem *flickable = nullptr;
};

// Counts the frames whose rows were not all there to draw.
class FrameWatch {
public:
    explicit FrameWatch(const Diagnostics &log)
        : m_connection(QObject::connect(
              log.window, &QQuickWindow::beforeSynchronizing, log.window,
              [this, &log]() {
                  ++m_frames;
                  const DiagnosticsLogView &view = *log.view;
                  if (view.length() == 0 || view.width() <= 0 || view.height() <= 0) {
                      return;
                  }
                  const qreal height = view.m_layout.rowHeight();
                  const int rows = view.m_layout.rowCount();
                  const qreal top = view.contentY() - view.topPadding();
                  const int first = qBound(0, static_cast<int>(std::floor(top / height)), rows - 1);
                  const int last = qBound(0, static_cast<int>(std::floor((top + view.height() - 1) / height)), rows - 1);
                  for (int row = first; row <= last; ++row) {
                      if (view.m_shown.count(row) == 0) {
                          ++m_incomplete;
                          return;
                      }
                  }
              },
              Qt::DirectConnection)) {}
    ~FrameWatch() { QObject::disconnect(m_connection); }
    FrameWatch(const FrameWatch &) = delete;
    FrameWatch &operator=(const FrameWatch &) = delete;

    int frames() const { return m_frames; }
    int incomplete() const { return m_incomplete; }

private:
    int m_frames = 0;
    int m_incomplete = 0;
    QMetaObject::Connection m_connection;
};

QString clipboardText() {
    return QGuiApplication::clipboard()->text();
}

} // namespace

class DiagnosticsLogViewTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void aFullLogShapesOnlyTheRowsThatShow();
    void closedItCostsAndKeepsNothing();
    void copiesWhatATextEditCopies();
    void copyActionsCopyTheLogsText();
    void selectsByMouse();
    void selectsByKeyboard();
    void scrollsByKeysAndWheel();
    void opensAndReloadsAtTheTop();
    void showsMarkupAsText();
    void selectionIsDrawnInTheThemesColours();
    void readsAsTextToAssistiveTechnology();
};

void DiagnosticsLogViewTest::initTestCase() {
    startCapturingMessages();
    FerrousLocalization::setUpApplicationDomain();
    registerFerrousQmlTypes();
}

void DiagnosticsLogViewTest::init() {
    clearCapturedMessages();
    movePointerOffTheWindows();
}

void DiagnosticsLogViewTest::cleanup() {
    dropCapturedMessages(offscreenSizeHintsWarning());
    const QString messages = takeCapturedMessagesText();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void DiagnosticsLogViewTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

// #724: a full log of long records took 250 ms to lay out, as one document,
// on open and on each Reload. Counted rather than timed: each change lays
// the rows out once, and shapes about a view's worth of them.
void DiagnosticsLogViewTest::aFullLogShapesOnlyTheRowsThatShow() {
    Diagnostics log;
    QVERIFY(log.loaded());
    QStringList lines = records(kFullLog, 1000);
    log.setLog(lines.join(QLatin1Char('\n')));
    const FrameWatch watch(log);
    QVERIFY(log.open());
    QCOMPARE(log.view->length(), lines.join(QLatin1Char('\n')).size());
    QVERIFY(log.view->layoutPasses() >= 1 && log.view->layoutPasses() <= 2);
    QVERIFY2(log.view->shapedRows() <= 3 * log.rowsInView(), qPrintable(QString::number(log.view->shapedRows())));

    // Reload, and lines appended to a log that is full.
    for (int change = 0; change < 3; ++change) {
        const int passes = log.view->layoutPasses();
        const int shaped = log.view->shapedRows();
        lines.removeFirst();
        lines.append(record(kFullLog + change, 1000));
        log.setLog(lines.join(QLatin1Char('\n')));
        QVERIFY(log.frame());
        QCOMPARE(log.view->layoutPasses(), passes + 1);
        QVERIFY(log.view->shapedRows() - shaped <= log.rowsInView() + 1);
    }

    // Scrolled to anywhere, a view's worth again, and no layout.
    const int passes = log.view->layoutPasses();
    for (const qreal part : {0.25, 0.5, 1.0}) {
        const int shaped = log.view->shapedRows();
        log.scrollTo(part * (log.view->contentHeight() - log.view->height()));
        QVERIFY(log.frame());
        QVERIFY(log.view->shapedRows() - shaped <= log.rowsInView() + 1);
        QVERIFY(static_cast<int>(log.view->m_shown.size()) <= log.rowsInView() + 1);
    }
    QCOMPARE(log.view->layoutPasses(), passes);
    QVERIFY(watch.frames() >= 6);
    QCOMPARE(watch.incomplete(), 0);
}

// The text edit took, and laid out, every change of the log while
// Diagnostics was closed.
void DiagnosticsLogViewTest::closedItCostsAndKeepsNothing() {
    Diagnostics log;
    QVERIFY(log.loaded());
    const QString text = fullLog();
    log.setLog(text);
    QTest::qWait(0);
    QCOMPARE(log.view->length(), 0);
    QCOMPARE(log.view->layoutPasses(), 0);
    QCOMPARE(log.view->shapedRows(), 0);

    int shapedPerOpen = -1;
    for (int opening = 0; opening < 6; ++opening) {
        const int shaped = log.view->shapedRows();
        QVERIFY(log.open());
        QTRY_VERIFY(log.view->length() == text.size());
        // After the first, which lays the window out, each costs the same.
        if (opening > 1) {
            QCOMPARE(log.view->shapedRows() - shaped, shapedPerOpen);
        }
        shapedPerOpen = log.view->shapedRows() - shaped;
        log.close();
        QTRY_VERIFY(!log.window->isVisible());
        QCOMPARE(log.view->length(), 0);
        QVERIFY(log.view->m_shown.empty());
        QCOMPARE(log.view->m_layout.m_records.capacity(), std::size_t(1));
        QCOMPARE(log.view->m_layout.m_rows.capacity(), std::size_t(1));

        const int passes = log.view->layoutPasses();
        log.setLog(text);
        QCOMPARE(log.view->length(), 0);
        QCOMPARE(log.view->layoutPasses(), passes);
    }
}

// Byte for byte, the plain text edit's copy of the same selection.
void DiagnosticsLogViewTest::copiesWhatATextEditCopies() {
    Diagnostics log;
    QVERIFY(log.loaded());
    QQmlComponent component(&log.engine);
    component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText; readOnly: true }", QUrl());
    const std::unique_ptr<QObject> edit(component.create());
    QVERIFY2(edit != nullptr, qPrintable(component.errorString()));

    QRandomGenerator random(724);
    for (const QString &text : {trickyLog(), QStringLiteral("one"), records(20, 220).join(QLatin1Char('\n')),
                                QStringLiteral("ends in a break\n"), QStringLiteral("\nstarts with one")}) {
        log.setLog(text);
        QVERIFY(log.open());
        QVERIFY(edit->setProperty("text", text));
        const int length = edit->property("length").toInt();
        QCOMPARE(log.view->length(), length);
        for (int round = 0; round < 80; ++round) {
            const int start = round == 0 ? 0 : random.bounded(length + 1);
            const int end = round == 0 ? length : random.bounded(length + 1);
            if (start == end) {
                continue;
            }
            QVERIFY(QMetaObject::invokeMethod(edit.get(), "select", Q_ARG(int, qMin(start, end)),
                                              Q_ARG(int, qMax(start, end))));
            QVERIFY(QMetaObject::invokeMethod(edit.get(), "copy"));
            const QString expected = clipboardText();
            QGuiApplication::clipboard()->clear();
            log.view->select(start, end);
            log.view->copy();
            QCOMPARE(clipboardText().toUtf8(), expected.toUtf8());
        }
    }
}

// Ctrl+C, the toolbar and the context menu.
void DiagnosticsLogViewTest::copyActionsCopyTheLogsText() {
    Diagnostics log;
    QVERIFY(log.loaded());
    const QString text = records(80, 600).join(QLatin1Char('\n'));
    log.setLog(text);
    QVERIFY(log.open());

    log.view->select(5, 1300);
    QTest::keyClick(log.window, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboardText(), text.mid(5, 1295));

    QGuiApplication::clipboard()->clear();
    QQuickItem *copyAll = itemNamed(log.window->contentItem(), QStringLiteral("diagnosticsCopyAllButton"));
    QVERIFY(copyAll != nullptr);
    QVERIFY(QMetaObject::invokeMethod(copyAll, "click"));
    QCOMPARE(clipboardText(), text);

    // The context menu's items, in its order.
    QObject *menu = nullptr;
    for (QObject *child : log.root->findChildren<QObject *>()) {
        if (child->inherits("QQuickMenu") && child->property("count").toInt() == 4) {
            menu = child;
        }
    }
    QVERIFY(menu != nullptr);
    const auto trigger = [menu](int index) {
        QQuickItem *item = nullptr;
        return QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, index))
            && item != nullptr && item->isEnabled() && QMetaObject::invokeMethod(item, "triggered");
    };
    log.view->select(100, 900);
    QVERIFY(trigger(0));
    QCOMPARE(clipboardText(), text.mid(100, 800));
    QVERIFY(trigger(1));
    QCOMPARE(log.view->selectedText(), text);
    log.view->select(0, 3);
    QVERIFY(trigger(2));
    QCOMPARE(clipboardText(), text);
    // Made of the bridge's own text, never of the rows.
    QVERIFY(trigger(3));
    QCOMPARE(log.bridge->property("copiesWithoutPaths").toInt(), 1);
    QCOMPARE(log.bridge->property("diagnosticsText").toString(), text);
}

void DiagnosticsLogViewTest::selectsByMouse() {
    Diagnostics log;
    QVERIFY(log.loaded());
    const QString text = records(400, 70).join(QLatin1Char('\n'));
    log.setLog(text);
    QVERIFY(log.open());
    const int perRecord = 71;

    // A drag across records.
    const int from = perRecord + 10;
    const int to = 3 * perRecord + 20;
    QTest::mousePress(log.window, Qt::LeftButton, Qt::NoModifier, log.inWindow(from));
    QTest::mouseMove(log.window, log.inWindow(to));
    QTest::mouseRelease(log.window, Qt::LeftButton, Qt::NoModifier, log.inWindow(to));
    QCOMPARE(log.view->selectedText(), text.mid(from, to - from));
    QVERIFY(log.view->hasActiveFocus());

    // Shift+click keeps the start.
    const int further = 5 * perRecord + 30;
    QTest::mouseClick(log.window, Qt::LeftButton, Qt::ShiftModifier, log.inWindow(further));
    QCOMPARE(log.view->selectedText(), text.mid(from, further - from));

    // A double click takes the word, a third click the record.
    const int word = 6 * perRecord + static_cast<int>(text.mid(6 * perRecord).indexOf(QStringLiteral("info"))) + 1;
    QTest::mouseDClick(log.window, Qt::LeftButton, Qt::NoModifier, log.inWindow(word));
    QCOMPARE(log.view->selectedText(), QStringLiteral("info"));
    QTest::mouseClick(log.window, Qt::LeftButton, Qt::NoModifier, log.inWindow(word));
    QCOMPARE(log.view->selectedText(), text.mid(6 * perRecord, perRecord - 1));

    // Held past the bottom, a drag scrolls and selects what it reveals.
    const QPoint start = log.inWindow(8 * perRecord);
    const QPoint below = log.view->mapToScene(QPointF(40, log.view->height() + 12)).toPoint();
    QTest::mousePress(log.window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(log.window, below);
    QTRY_VERIFY(log.contentY() > log.view->height());
    QTRY_VERIFY(log.view->selectionEnd() > 60 * perRecord);
    QTest::mouseRelease(log.window, Qt::LeftButton, Qt::NoModifier, below);
    QCOMPARE(log.view->selectionStart(), 8 * perRecord);
    // It stops with the button.
    const qreal stopped = log.contentY();
    QTest::qWait(80);
    QCOMPARE(log.contentY(), stopped);
}

void DiagnosticsLogViewTest::selectsByKeyboard() {
    Diagnostics log;
    QVERIFY(log.loaded());
    const QString text = records(300, 70).join(QLatin1Char('\n'));
    log.setLog(text);
    QVERIFY(log.open());
    const int perRecord = 71;

    QTest::keyClick(log.window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(log.window, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(log.view->selectedText(), text.left(2));
    QTest::keyClick(log.window, Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(log.view->selectedText(), text.left(1));
    QTest::keyClick(log.window, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(log.view->selectedText(), text.left(perRecord + 1));
    QTest::keyClick(log.window, Qt::Key_Up, Qt::ShiftModifier);
    QCOMPARE(log.view->selectedText(), text.left(1));
    QTest::keyClick(log.window, Qt::Key_End, Qt::ShiftModifier);
    QCOMPARE(log.view->selectedText(), text.left(perRecord - 1));
    // On over the newline, as one character.
    QTest::keyClick(log.window, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(log.view->selectedText(), text.left(perRecord));
    QTest::keyClick(log.window, Qt::Key_End, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(log.view->selectedText(), text);
    QTRY_COMPARE(log.contentY(), log.view->contentHeight() - log.view->height());

    log.view->select(0, 0);
    QTest::keyClick(log.window, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(log.view->selectedText(), text);
    QCOMPARE(log.view->cursorPosition(), text.size());
}

void DiagnosticsLogViewTest::scrollsByKeysAndWheel() {
    Diagnostics log;
    QVERIFY(log.loaded());
    log.setLog(records(300, 70).join(QLatin1Char('\n')));
    QVERIFY(log.open());
    QVERIFY(log.view->hasActiveFocus());
    const qreal line = log.view->m_layout.rowHeight();
    const qreal bottom = log.view->contentHeight() - log.view->height();

    QTest::keyClick(log.window, Qt::Key_Down);
    QTRY_COMPARE(log.contentY(), line);
    QTest::keyClick(log.window, Qt::Key_Up);
    QTRY_COMPARE(log.contentY(), 0.0);
    QTest::keyClick(log.window, Qt::Key_PageDown);
    QTRY_COMPARE(log.contentY(), log.view->height() - line);
    QTest::keyClick(log.window, Qt::Key_PageUp);
    QTRY_COMPARE(log.contentY(), 0.0);
    QTest::keyClick(log.window, Qt::Key_End);
    QTRY_COMPARE(log.contentY(), bottom);
    QTest::keyClick(log.window, Qt::Key_Home);
    QTRY_COMPARE(log.contentY(), 0.0);
    QVERIFY(!log.view->hasSelection());

    const QPointF over = log.view->mapToScene(QPointF(log.view->width() / 2, log.view->height() / 2));
    QWheelEvent wheel(over, log.window->mapToGlobal(over), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(log.window, &wheel);
    QTRY_VERIFY(log.contentY() > 0);

    QTest::keyClick(log.window, Qt::Key_Tab);
    QTRY_VERIFY(!log.view->hasActiveFocus());
    QQuickItem *closeButton = log.window->activeFocusItem();
    QVERIFY(closeButton != nullptr);
    QCOMPARE(closeButton->objectName(), QStringLiteral("diagnosticsCloseButton"));
    QTest::keyClick(log.window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(log.view->hasActiveFocus());
}

// As the text edit had it: at the top on open and on a Reload with a new
// text, where it was on one with the same.
void DiagnosticsLogViewTest::opensAndReloadsAtTheTop() {
    Diagnostics log;
    QVERIFY(log.loaded());
    QStringList lines = records(300, 70);
    log.setLog(lines.join(QLatin1Char('\n')));
    QVERIFY(log.open());
    QCOMPARE(log.contentY(), 0.0);

    log.scrollTo(900);
    log.view->select(3000, 3050);
    log.setLog(lines.join(QLatin1Char('\n')));
    QVERIFY(log.frame());
    QCOMPARE(log.contentY(), 900.0);
    QCOMPARE(log.view->selectionEnd(), 3050);

    lines.append(QStringLiteral("[app] info: appended"));
    log.setLog(lines.join(QLatin1Char('\n')));
    QVERIFY(log.frame());
    QCOMPARE(log.contentY(), 0.0);
    QVERIFY(!log.view->hasSelection());

    log.scrollTo(900);
    log.close();
    QTRY_VERIFY(!log.window->isVisible());
    QVERIFY(log.open());
    QCOMPARE(log.contentY(), 0.0);
}

void DiagnosticsLogViewTest::showsMarkupAsText() {
    Diagnostics log;
    QVERIFY(log.loaded());
    const QString line = QStringLiteral("[app] <b>not bold</b> &lt;path&gt;");
    log.setLog(line);
    QVERIFY(log.open());
    QCOMPARE(log.view->m_shown.size(), std::size_t(1));
    const QTextLayout &drawn = *log.view->m_shown.begin()->second.layout;
    QCOMPARE(drawn.text(), line);
    QVERIFY(drawn.formats().isEmpty());
    // Each of the tags' characters has its cell.
    QVERIFY(qAbs(drawn.lineAt(0).cursorToX(static_cast<int>(line.size()))
                 - line.size() * log.view->m_layout.cellWidth())
            < 1);
}

void DiagnosticsLogViewTest::selectionIsDrawnInTheThemesColours() {
    Diagnostics log;
    QVERIFY(log.loaded());
    const QString text = records(5, 60).join(QLatin1Char('\n'));
    log.setLog(text);
    QVERIFY(log.open());
    // From inside the first record to inside the third, to the character:
    // QSGTextNode's own selection took one more.
    const int from = 5;
    const int to = 2 * 61 + 20;
    log.view->select(from, to);
    QVERIFY(log.frame());
    const QImage image = log.window->grabWindow();
    const QRgb band = log.view->selectionColor().rgb();
    for (const int position : {from - 1, from, 60 - 1, 61, 2 * 61 - 2, 2 * 61, to - 1, to, 3 * 61 + 2}) {
        const QRectF at = log.view->positionToRectangle(position);
        // Inside the character's cell.
        const QRect cell = QRectF(log.view->mapToScene(at.topLeft() + QPointF(1, 0)),
                                  QSizeF(log.view->m_layout.cellWidth() - 2, at.height()))
                               .toRect();
        bool banded = false;
        for (int x = cell.left(); x <= cell.right() && !banded; ++x) {
            for (int y = cell.top(); y <= cell.bottom() && !banded; ++y) {
                banded = image.pixel(x, y) == band;
            }
        }
        QVERIFY2(banded == (position >= from && position < to), qPrintable(QString::number(position)));
    }
}

void DiagnosticsLogViewTest::readsAsTextToAssistiveTechnology() {
    QAccessible::setActive(true);
    const QScopeGuard stop([]() { QAccessible::setActive(false); });
    Diagnostics log;
    QVERIFY(log.loaded());
    const QString text = QStringLiteral("[app] first record\n[ui] second record\n[ui] third");
    log.setLog(text);
    QVERIFY(log.open());

    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(log.view);
    QVERIFY(accessible != nullptr);
    QCOMPARE(accessible->role(), QAccessible::EditableText);
    QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Log"));
    QCOMPARE(accessible->text(QAccessible::Value), text);
    QVERIFY(accessible->state().readOnly && accessible->state().multiLine && accessible->state().focused);
    QVERIFY(!accessible->rect().isEmpty());

    QAccessibleTextInterface *reader = accessible->textInterface();
    QVERIFY(reader != nullptr);
    QCOMPARE(reader->characterCount(), text.size());
    QCOMPARE(reader->text(0, reader->characterCount()), text);
    int start = -1;
    int end = -1;
    QCOMPARE(reader->textAtOffset(25, QAccessible::ParagraphBoundary, &start, &end),
             QStringLiteral("[ui] second record\n"));
    QCOMPARE(reader->textBeforeOffset(25, QAccessible::LineBoundary, &start, &end),
             QStringLiteral("[app] first record\n"));
    QCOMPARE(reader->textAfterOffset(25, QAccessible::LineBoundary, &start, &end), QStringLiteral("[ui] third"));
    QCOMPARE(reader->textAtOffset(8, QAccessible::WordBoundary, &start, &end), QStringLiteral("first"));
    reader->setSelection(0, 6, 11);
    QCOMPARE(log.view->selectedText(), QStringLiteral("first"));
    QCOMPARE(reader->selectionCount(), 1);
    QVERIFY(reader->characterRect(7).width() > 1);
    QCOMPARE(reader->offsetAtPoint(reader->characterRect(7).topLeft() + QPoint(1, 2)), 7);

    // In the window's tree, from either end.
    QAccessibleInterface *windowAccessible = QAccessible::queryAccessibleInterface(log.window);
    const std::function<bool(QAccessibleInterface *)> holds = [&](QAccessibleInterface *from) {
        if (from == accessible) {
            return true;
        }
        for (int index = 0; index < from->childCount(); ++index) {
            QAccessibleInterface *child = from->child(index);
            if (child != nullptr && holds(child)) {
                return true;
            }
        }
        return false;
    };
    QVERIFY(holds(windowAccessible));
    QAccessibleInterface *up = accessible->parent();
    QVERIFY(up != nullptr && up->indexOfChild(accessible) >= 0);
    for (int depth = 0; depth < 30 && up != nullptr && up != windowAccessible; ++depth) {
        up = up->parent();
    }
    QCOMPARE(up, windowAccessible);
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    isolateFromDesktopSessionWithAppLook();
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    if (qgetenv("QT_QPA_PLATFORM") == QByteArrayLiteral("offscreen")) {
        const QString screens = fixtures.filePath(QStringLiteral("offscreen-screens.json"));
        QFile file(screens);
        if (!file.open(QIODevice::WriteOnly)
            || file.write(R"({"screens": [{"name": "Desktop", "x": 0, "y": 0, "width": 2560, "height": 1600,
                "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]})") < 0) {
            return 1;
        }
        file.close();
        qputenv("QT_QPA_PLATFORM", QFile::encodeName(QStringLiteral("offscreen:configfile=") + screens));
    }
    QApplication app(argc, argv);
    returnFocusFromClosedPopups();
    DiagnosticsLogViewTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_diagnostics_log_view.moc"
