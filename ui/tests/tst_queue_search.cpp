// SPDX-License-Identifier: GPL-3.0-or-later

// Find in Queue (#814): the results model, a view of the queue's rows that
// follows them through the queue's edits, and the palette's rows and keys:
// Enter plays the row found without replacing the queue, Ctrl+Enter marks
// it to play next, Alt+Enter shows it in the queue, and Esc closes.

#include <QApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <functional>
#include <memory>

#include "Localization.h"
#include "QueueRows.h"
#include "QueueSearchResultsModel.h"
#include "TestMessages.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

QueueRowData queueRow(const QString &title, const QString &path, const QString &artist = QString()) {
    QueueRowData row;
    row.title = title;
    row.artist = artist;
    row.album = QStringLiteral("Fixture Album");
    row.path = path;
    row.lengthText = QStringLiteral("3:21");
    return row;
}

// A queue of `titles`, each in a file of its own, but "Beta" twice in one.
QVector<QueueRowData> queueOf(const QStringList &titles) {
    QVector<QueueRowData> rows;
    for (const QString &title : titles) {
        rows.push_back(queueRow(title, QStringLiteral("/fixture/%1.flac").arg(title.toLower())));
    }
    return rows;
}

QVector<int> queueRowsOf(const QueueSearchResultsModel &model) {
    QVector<int> rows;
    for (int i = 0; i < model.rowCount(); ++i) {
        rows.push_back(model.index(i, 0).data(QueueSearchResultsModel::QueueRowRole).toInt());
    }
    return rows;
}

QStringList titlesOf(const QueueSearchResultsModel &model) {
    QStringList titles;
    for (int i = 0; i < model.rowCount(); ++i) {
        titles.push_back(model.index(i, 0).data(QueueSearchResultsModel::TitleRole).toString());
    }
    return titles;
}

QString formatErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    for (const QQmlError &error : errors) {
        lines << error.toString();
    }
    return lines.join(QLatin1Char('\n'));
}

// A window with a transport band, the palette, and a fake bridge over the
// test's own queue and results models.
const char *kHarnessQml = R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "dialogs" as Dialogs

ApplicationWindow {
    id: window
    width: 1400
    height: 900
    visible: true
    property alias bridge: bridge
    property alias dialog: dialog
    property var shown: []
    property int queueFocused: 0
    QtObject {
        id: bridge
        property var queueSearchModel: searchModel
        property bool queueSearchBusy: false
        property int queueLength: queueModel.rowCount()
        property int playingQueueIndex: -1
        property string playbackState: "Stopped"
        property var queries: []
        property var commands: []
        function setQueueSearchQuery(query) { queries = queries.concat([query]) }
        function jumpToQueueRow(row) { commands = commands.concat(["jump to " + row]) }
        function togglePlayNext(rows) { commands = commands.concat(["play next " + rows.join(",")]) }
    }
    Components.UiPalette {
        id: tokens
        windowRoot: window
    }
    Rectangle {
        id: transport
        width: parent.width
        height: 48
    }
    Dialogs.QueueSearchDialog {
        id: dialog
        uiBridge: bridge
        uiPalette: tokens
        windowRoot: window
        anchorItem: transport
        showQueueRow: function(row) { window.shown = window.shown.concat([row]) }
        focusQueue: function() { window.queueFocused += 1 }
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
    }
}
)QML";

void visit(QQuickItem *item, const std::function<void(QQuickItem *)> &function) {
    function(item);
    for (QQuickItem *child : item->childItems()) {
        visit(child, function);
    }
}

QList<QQuickItem *> visibleNamed(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> items;
    visit(root, [&](QQuickItem *item) {
        if (item->objectName() == name && item->isVisible()) {
            items << item;
        }
    });
    return items;
}

struct Harness {
    QueueRowsModel queue;
    QueueSearchResultsModel results{&queue};
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QObject *bridge = nullptr;
    QObject *dialog = nullptr;

    bool load(QString *errorText) {
        engine.rootContext()->setContextProperty(QStringLiteral("searchModel"), &results);
        engine.rootContext()->setContextProperty(QStringLiteral("queueModel"), &queue);
        QQmlComponent component(&engine);
        component.setData(kHarnessQml,
                          QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/QueueSearchHarness.qml")));
        root.reset(component.create());
        if (!root) {
            *errorText = formatErrors(component.errors());
            return false;
        }
        window = qobject_cast<QQuickWindow *>(root.get());
        bridge = root->property("bridge").value<QObject *>();
        dialog = root->property("dialog").value<QObject *>();
        if (window == nullptr || bridge == nullptr || dialog == nullptr || !QTest::qWaitForWindowExposed(window)) {
            *errorText = QStringLiteral("the harness did not show");
            return false;
        }
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window)) {
            *errorText = QStringLiteral("the harness did not become active");
            return false;
        }
        dialog->setProperty("fadeDuration", 0);
        return true;
    }

    QQuickItem *item(const QString &name) const {
        const auto items = visibleNamed(window->contentItem(), name);
        return items.isEmpty() ? nullptr : items.first();
    }
    // Also while the palette is closed.
    QQuickItem *field() const { return dialog->findChild<QQuickItem *>(QStringLiteral("queueSearchQueryField")); }
    QList<QQuickItem *> rows() const {
        QList<QQuickItem *> rows = visibleNamed(window->contentItem(),
                                                 QStringLiteral("queueSearchResultRow"));
        std::sort(rows.begin(), rows.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->property("index").toInt() < b->property("index").toInt();
        });
        return rows;
    }

    bool open() {
        QMetaObject::invokeMethod(dialog, "openPalette");
        return QTest::qWaitFor([this] { return dialog->property("opened").toBool(); }) && field() != nullptr
            && field()->hasActiveFocus();
    }
    // Types `query`, and shows `found` as its results, as the bridge does
    // once its search ends.
    void search(const QString &query, const QVector<int> &found) {
        bridge->setProperty("queueSearchBusy", true);
        field()->setProperty("text", query);
        results.setRows(found);
        bridge->setProperty("queueSearchBusy", false);
    }
    QStringList strings(const char *name) const { return bridge->property(name).toStringList(); }
    QString takeCommands() {
        const QString taken = strings("commands").join(QStringLiteral("; "));
        bridge->setProperty("commands", QStringList());
        return taken;
    }
    int selected() const { return dialog->property("selectedIndex").toInt(); }
    QStringList hints() const {
        QStringList pairs;
        for (QQuickItem *hint : visibleNamed(window->contentItem(), QStringLiteral("queueSearchKeyHint"))) {
            pairs << hint->property("keyText").toString() + QLatin1Char(' ') + hint->property("actionText").toString();
        }
        return pairs;
    }
};

QString textOf(QQuickItem *row, const QString &name) {
    const auto items = visibleNamed(row, name);
    return items.isEmpty() ? QString() : items.first()->property("text").toString();
}

} // namespace

class QueueSearchTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void resultsFollowTheirRowsThroughTheQueuesEdits();
    void resultsShowTheirRowsDetailsAndMarks();
    void paletteSearchesOnlyWhileOpenAndShowsRowsInQueueOrder();
    void enterPlaysTheRowFoundAndCloses();
    void ctrlEnterMarksTheRowToPlayNextAndStaysOpen();
    void altEnterShowsTheRowInTheQueue();
    void keysPressedDuringASearchActOnItsResults();
    void thePlayingRowIsMarkedNotOtherRowsOfItsFile();
    void theSelectionStaysOnItsRowThroughTheQueuesEdits();
};

void QueueSearchTest::initTestCase() {
    startCapturingMessages();
    QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("icons/breeze/index.theme"))
                  .isEmpty(),
             "The Breeze icon theme is missing: install breeze-icon-theme (docs/INSTALL.md)");
}

void QueueSearchTest::cleanup() {
    const QStringList list = takeCapturedMessages();
    QVERIFY2(list.isEmpty(), qPrintable(list.join(QLatin1Char('\n'))));
}

void QueueSearchTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

// A result keeps to its row: rows put before it move it down, a removal
// before it moves it up, a move takes it along and keeps the results in
// queue order, and a result whose row is removed leaves them. Rows the
// queue gains are no results until a search finds them, and a whole new
// queue leaves none.
void QueueSearchTest::resultsFollowTheirRowsThroughTheQueuesEdits() {
    QueueRowsModel queue;
    queue.setRows(queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma"),
                           QStringLiteral("Delta"), QStringLiteral("Epsilon")}));
    QueueSearchResultsModel results(&queue);
    QSignalSpy count(&results, &QueueSearchResultsModel::countChanged);
    // Rows the queue does not have are left out.
    results.setRows({1, 3, 9});
    QCOMPARE(queueRowsOf(results), (QVector<int>{1, 3}));
    QCOMPARE(count.count(), 1);
    QCOMPARE(results.indexOfQueueRow(3), 1);
    QCOMPARE(results.indexOfQueueRow(2), -1);
    QCOMPARE(results.queueRowAt(5), -1);

    QSignalSpy changed(&results, &QAbstractItemModel::dataChanged);
    queue.insertRowsAt(0, {queueRow(QStringLiteral("New"), QStringLiteral("/fixture/new.flac"))});
    QCOMPARE(queueRowsOf(results), (QVector<int>{2, 4}));
    QCOMPARE(titlesOf(results), (QStringList{QStringLiteral("Beta"), QStringLiteral("Delta")}));
    QCOMPARE(changed.count(), 1);

    // Delta moves to the top, before Beta: the results follow the queue.
    QSignalSpy moves(&results, &QAbstractItemModel::rowsMoved);
    QVERIFY(queue.moveRow(4, 0));
    QCOMPARE(moves.count(), 1);
    // Delta's result, the second, goes before the first.
    QCOMPARE(moves[0][1].toInt(), 1);
    QCOMPARE(moves[0][2].toInt(), 1);
    QCOMPARE(moves[0][4].toInt(), 0);
    QCOMPARE(queueRowsOf(results), (QVector<int>{0, 3}));
    QCOMPARE(titlesOf(results), (QStringList{QStringLiteral("Delta"), QStringLiteral("Beta")}));
    // A move that keeps their order changes only their places.
    QVERIFY(queue.moveRows(1, 2, 4));
    QCOMPARE(moves.count(), 1);
    QCOMPARE(titlesOf(results), (QStringList{QStringLiteral("Delta"), QStringLiteral("Beta")}));
    QCOMPARE(queueRowsOf(results), (QVector<int>{0, 1}));

    // Beta's row goes: it leaves the results, and nothing takes its place.
    QSignalSpy removed(&results, &QAbstractItemModel::rowsRemoved);
    QVERIFY(queue.removeRowsAt(1, 1));
    QCOMPARE(removed.count(), 1);
    QCOMPARE(titlesOf(results), (QStringList{QStringLiteral("Delta")}));
    QCOMPARE(count.count(), 2);

    queue.setRows(queueOf({QStringLiteral("Other")}));
    QCOMPARE(results.rowCount(), 0);
    QCOMPARE(count.count(), 3);
}

void QueueSearchTest::resultsShowTheirRowsDetailsAndMarks() {
    QueueRowsModel queue;
    auto rows = queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta")});
    rows[1].artist = QStringLiteral("Fixture Artist");
    rows[1].coverPath = QStringLiteral("/fixture/cover.png");
    rows[1].coverStamp = 7;
    queue.setRows(rows);
    QueueSearchResultsModel results(&queue);
    results.setRows({1});
    const QVariantMap data = results.rowDataAt(0);
    QCOMPARE(data.value(QStringLiteral("title")).toString(), QStringLiteral("Beta"));
    QCOMPARE(data.value(QStringLiteral("artist")).toString(), QStringLiteral("Fixture Artist"));
    QCOMPARE(data.value(QStringLiteral("lengthText")).toString(), QStringLiteral("3:21"));
    QCOMPARE(data.value(QStringLiteral("queueRow")).toInt(), 1);
    QCOMPARE(data.value(QStringLiteral("playNext")).toInt(), 0);
    // Through the image provider, which reads the cover off the GUI thread.
    QVERIFY(data.value(QStringLiteral("coverUrl")).toString().startsWith(QStringLiteral("image://")));
    QVERIFY(results.rowDataAt(1).isEmpty());

    // A mark tells the result of its row.
    QSignalSpy changed(&results, &QAbstractItemModel::dataChanged);
    queue.setPlayNext({1});
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed[0][2].value<QList<int>>(), QList<int>{QueueSearchResultsModel::PlayNextRole});
    QCOMPARE(results.rowDataAt(0).value(QStringLiteral("playNext")).toInt(), 1);
    // A mark of another row, after its own, tells it nothing.
    queue.setPlayNext({1, 0});
    QCOMPARE(changed.count(), 1);
    // A change of its row's details does.
    QueueRowData renamed = rows[1];
    renamed.title = QStringLiteral("Beta (Live)");
    QVERIFY(queue.updateRow(1, renamed));
    QCOMPARE(changed.count(), 2);
    QCOMPARE(titlesOf(results), (QStringList{QStringLiteral("Beta (Live)")}));

    // Its matches, as global search cuts them.
    const QVariantList segments = results.highlightSegments(QStringLiteral("Night Café"), QStringLiteral("cafe"));
    QCOMPARE(segments.size(), 2);
    QCOMPARE(segments[1].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Café"));
    QVERIFY(segments[1].toMap().value(QStringLiteral("match")).toBool());
}

// The palette asks the bridge for its query only while open, and for none
// once closed. Its rows are the queue rows found, in queue order, each with
// its place in the queue, the first selected; with none, it says so.
void QueueSearchTest::paletteSearchesOnlyWhileOpenAndShowsRowsInQueueOrder() {
    Harness harness;
    QString error;
    QVERIFY2(harness.load(&error), qPrintable(error));
    harness.queue.setRows(queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma"),
                                   QStringLiteral("Beta Two"), QStringLiteral("Delta")}));
    QVERIFY(harness.open());
    QCOMPARE(harness.strings("queries"), QStringList{QString()});
    QCOMPARE(harness.dialog->property("title").toString(), QStringLiteral("Find in Queue"));
    QCOMPARE(harness.hints(), QStringList{QStringLiteral("Esc Close")});

    harness.search(QStringLiteral("beta"), {1, 3});
    QCOMPARE(harness.strings("queries").last(), QStringLiteral("beta"));
    QTRY_COMPARE(harness.rows().size(), 2);
    const auto rows = harness.rows();
    QCOMPARE(textOf(rows[0], QStringLiteral("queueSearchResultPlace")), QStringLiteral("2"));
    QCOMPARE(textOf(rows[1], QStringLiteral("queueSearchResultPlace")), QStringLiteral("4"));
    QCOMPARE(harness.selected(), 0);
    QCOMPARE(harness.hints(), (QStringList{QStringLiteral("Enter Play"), QStringLiteral("Ctrl+Enter Play Next"),
                                           QStringLiteral("Alt+Enter Show in Queue"), QStringLiteral("Esc Close")}));
    // Matches in the title are semibold.
    bool semibold = false;
    for (QQuickItem *run : visibleNamed(rows[0], QStringLiteral("queueSearchResultTitle"))) {
        if (run->property("text").toString() == QStringLiteral("Beta")) {
            semibold = run->property("font").value<QFont>().weight() == QFont::DemiBold;
        }
    }
    QVERIFY(semibold);

    harness.search(QStringLiteral("nothing"), {});
    QTRY_VERIFY(harness.item(QStringLiteral("queueSearchNoResults")) != nullptr);
    QCOMPARE(textOf(harness.item(QStringLiteral("queueSearchNoResults")), QStringLiteral("queueSearchNoResultsTitle")),
             QStringLiteral("No matches for “nothing” in the queue"));

    QTest::keyClick(harness.window, Qt::Key_Escape);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
    QCOMPARE(harness.strings("queries").last(), QString());
    // Typing while closed searches nothing.
    harness.field()->setProperty("text", QStringLiteral("gamma"));
    QCOMPARE(harness.strings("queries").last(), QString());
    QVERIFY(harness.takeCommands().isEmpty());
}

void QueueSearchTest::enterPlaysTheRowFoundAndCloses() {
    Harness harness;
    QString error;
    QVERIFY2(harness.load(&error), qPrintable(error));
    harness.queue.setRows(queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma"),
                                   QStringLiteral("Beta Two")}));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("beta"), {1, 3});
    QTRY_COMPARE(harness.rows().size(), 2);
    QTest::keyClick(harness.window, Qt::Key_Down);
    QCOMPARE(harness.selected(), 1);
    // Down from the last wraps to the first, and Up back.
    QTest::keyClick(harness.window, Qt::Key_Down);
    QCOMPARE(harness.selected(), 0);
    QTest::keyClick(harness.window, Qt::Key_Up);
    QCOMPARE(harness.selected(), 1);
    QTest::keyClick(harness.window, Qt::Key_Return);
    QCOMPARE(harness.takeCommands(), QStringLiteral("jump to 3"));
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
    // The queue's selection is not the palette's to change.
    QVERIFY(harness.root->property("shown").toList().isEmpty());

    // Once a row moved, Enter plays it where it is now.
    QVERIFY(harness.open());
    harness.search(QStringLiteral("gamma"), {2});
    QTRY_COMPARE(harness.rows().size(), 1);
    harness.queue.insertRowsAt(0, {queueRow(QStringLiteral("First"), QStringLiteral("/fixture/first.flac"))});
    QTRY_COMPARE(textOf(harness.rows().value(0), QStringLiteral("queueSearchResultPlace")), QStringLiteral("4"));
    QTest::keyClick(harness.window, Qt::Key_Enter);
    QCOMPARE(harness.takeCommands(), QStringLiteral("jump to 3"));
}

void QueueSearchTest::ctrlEnterMarksTheRowToPlayNextAndStaysOpen() {
    Harness harness;
    QString error;
    QVERIFY2(harness.load(&error), qPrintable(error));
    harness.queue.setRows(queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")}));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("a"), {0, 1, 2});
    QTRY_COMPARE(harness.rows().size(), 3);
    QTest::keyClick(harness.window, Qt::Key_Down);
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(harness.takeCommands(), QStringLiteral("play next 1"));
    QVERIFY(harness.dialog->property("visible").toBool());

    // The mark shows on the row, and Ctrl+Enter then unmarks it.
    harness.queue.setPlayNext({1});
    QTRY_COMPARE(textOf(harness.rows().value(1), QStringLiteral("queueSearchResultPlayNext")), QStringLiteral("1"));
    QCOMPARE(harness.rows().value(1)->property("playNextText").toString(), QStringLiteral("plays next, number 1"));
    QTRY_VERIFY(harness.hints().contains(QStringLiteral("Ctrl+Enter Don’t Play Next")));
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(harness.takeCommands(), QStringLiteral("play next 1"));
    // The keypad's Enter too.
    QTest::keyClick(harness.window, Qt::Key_Enter, Qt::ControlModifier | Qt::KeypadModifier);
    QCOMPARE(harness.takeCommands(), QStringLiteral("play next 1"));
    QVERIFY(harness.dialog->property("visible").toBool());
}

void QueueSearchTest::altEnterShowsTheRowInTheQueue() {
    Harness harness;
    QString error;
    QVERIFY2(harness.load(&error), qPrintable(error));
    harness.queue.setRows(queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")}));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("gamma"), {2});
    QTRY_COMPARE(harness.rows().size(), 1);
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::AltModifier);
    QCOMPARE(harness.root->property("shown").toList(), QVariantList{2});
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
    // The queue has the keyboard once the palette has closed.
    QTRY_COMPARE(harness.root->property("queueFocused").toInt(), 1);
    QVERIFY(harness.takeCommands().isEmpty());

    // Escape closes without showing anything.
    QVERIFY(harness.open());
    QTest::keyClick(harness.window, Qt::Key_Escape);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
    QCOMPARE(harness.root->property("shown").toList(), QVariantList{2});
    QCOMPARE(harness.root->property("queueFocused").toInt(), 1);
}

// Enter pressed while the search for the query as typed runs plays the
// first row it finds, never a row of the older query's results (#723).
void QueueSearchTest::keysPressedDuringASearchActOnItsResults() {
    Harness harness;
    QString error;
    QVERIFY2(harness.load(&error), qPrintable(error));
    harness.queue.setRows(queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")}));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("alpha"), {0});
    QTRY_COMPARE(harness.rows().size(), 1);
    harness.bridge->setProperty("queueSearchBusy", true);
    harness.field()->setProperty("text", QStringLiteral("gamma"));
    QTest::keyClick(harness.window, Qt::Key_Return);
    QVERIFY(harness.takeCommands().isEmpty());
    harness.results.setRows({2});
    harness.bridge->setProperty("queueSearchBusy", false);
    QCOMPARE(harness.takeCommands(), QStringLiteral("jump to 2"));
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());

    // Editing the query drops the held key.
    QVERIFY(harness.open());
    harness.bridge->setProperty("queueSearchBusy", true);
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    harness.field()->setProperty("text", QStringLiteral("beta"));
    harness.results.setRows({1});
    harness.bridge->setProperty("queueSearchBusy", false);
    QVERIFY(harness.takeCommands().isEmpty());
}

// The playing track's row shows the play glyph, not another row of its
// file, and keeps it as the queue changes around it.
void QueueSearchTest::thePlayingRowIsMarkedNotOtherRowsOfItsFile() {
    Harness harness;
    QString error;
    QVERIFY2(harness.load(&error), qPrintable(error));
    auto rows = queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")});
    rows.push_back(rows[1]);
    harness.queue.setRows(rows);
    harness.bridge->setProperty("playingQueueIndex", 3);
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("beta"), {1, 3});
    QTRY_COMPARE(harness.rows().size(), 2);
    const auto nowIn = [&harness](int index) {
        return !visibleNamed(harness.rows().value(index), QStringLiteral("queueSearchNowGlyph")).isEmpty();
    };
    QTRY_VERIFY(nowIn(1));
    QVERIFY(!nowIn(0));
    // Stopped, no row is now.
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_VERIFY(!nowIn(1));
}

// Two results of one file swap places: the selection stays on its row,
// and a menu open on a result whose row goes is closed, so it acts on no
// other row.
void QueueSearchTest::theSelectionStaysOnItsRowThroughTheQueuesEdits() {
    Harness harness;
    QString error;
    QVERIFY2(harness.load(&error), qPrintable(error));
    auto rows = queueOf({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")});
    rows.insert(2, rows[1]);
    harness.queue.setRows(rows);
    QVERIFY(harness.open());
    harness.search(QStringLiteral("beta"), {1, 2});
    QTRY_COMPARE(harness.rows().size(), 2);
    QTest::keyClick(harness.window, Qt::Key_Down);
    QCOMPARE(harness.selected(), 1);
    // The selected row 2 moves up above row 1.
    QVERIFY(harness.queue.moveRow(2, 1));
    QCOMPARE(harness.selected(), 0);
    QCOMPARE(harness.results.queueRowAt(0), 1);
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(harness.takeCommands(), QStringLiteral("play next 1"));
    // A move that keeps the results' order keeps the selection.
    QVERIFY(harness.queue.moveRow(3, 0));
    QCOMPARE(harness.selected(), 0);
    QCOMPARE(harness.results.queueRowAt(0), 2);

    QObject *menu = harness.dialog->findChild<QObject *>(QStringLiteral("queueSearchResultMenu"));
    QVERIFY(menu != nullptr);
    QTest::keyClick(harness.window, Qt::Key_Menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(harness.queue.removeRowsAt(2, 1));
    QTRY_VERIFY(!menu->property("visible").toBool());
    QCOMPARE(harness.results.rowCount(), 1);
    QCOMPARE(harness.selected(), 0);
    QVERIFY(harness.takeCommands().isEmpty());
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    qputenv("QSG_RENDER_LOOP", "basic");
    isolateFromDesktopSessionWithAppLook();
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    qputenv("LANGUAGE", "en_US");
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QApplication app(argc, argv);
    returnFocusFromClosedPopups();
    FerrousLocalization::registerQmlTypes();
    QueueSearchTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_queue_search.moc"
