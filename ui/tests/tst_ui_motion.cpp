// SPDX-License-Identifier: GPL-3.0-or-later

// The motion of the main window (#373): the hand-over of amber on a track
// change in the queue and the library tree, the now-playing panel's
// cross-fades, the tree's expand and collapse, and the queue's reorder by
// drag. Each uses Kirigami's durations with an OutCubic curve, is instant
// with Plasma's animations off, and leaves nothing running once it is done.

#include <QApplication>
#include <QColor>
#include <QEasingCurve>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <Kirigami/Platform/Units>

#include <functional>

#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/QueueRows.h"
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
    const QString text = capturedMessages().join(QLatin1Char('\n'));
    capturedMessages().clear();
    return text;
}

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
    if (!QTest::qWaitFor([&]() { return window->width() == 1600 && window->height() == 1000; })) {
        return nullptr;
    }
    return window;
}

QObject *bridgeOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiBridge"));
}

QString hex(const QColor &color) {
    return color.name(QColor::HexArgb);
}

QString nowHex(QQuickWindow *window) {
    auto *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    return hex(palette->property("now").value<QColor>());
}

QString colorOf(const QObject *object) {
    return hex(object->property("color").value<QColor>());
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

QQuickItem *itemNamed(QQuickItem *root, const QString &name) {
    const QList<QQuickItem *> found = itemsNamed(root, name);
    return found.isEmpty() ? nullptr : found.first();
}

QQuickItem *named(QQuickWindow *window, const char *name) {
    return itemNamed(window->contentItem(), QString::fromLatin1(name));
}

QQuickItem *itemAtIndex(QQuickItem *view, int index) {
    QQuickItem *row = nullptr;
    QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
    return row;
}

// The engine's Kirigami.Units, whose durations Plasma's animation speed
// sets: 0 with animations off.
Kirigami::Platform::Units *unitsOf(QQuickWindow *window) {
    QQmlExpression expression(qmlContext(window), window, QStringLiteral("Kirigami.Units"));
    const QVariant value = expression.evaluate();
    return qobject_cast<Kirigami::Platform::Units *>(qvariant_cast<QObject *>(value));
}

QEasingCurve::Type easingOf(const QObject *animation) {
    return animation->property("easing").value<QEasingCurve>().type();
}

QObject *animationOf(QObject *owner) {
    return qvariant_cast<QObject *>(owner->property("animation"));
}

// The queue: five tracks of one album.
QVariantList queueRows() {
    QVariantList rows;
    for (int i = 0; i < 5; ++i) {
        rows.push_back(QVariantMap{
            {QStringLiteral("title"), QStringLiteral("Track %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist")},
            {QStringLiteral("album"), QStringLiteral("Album")},
            {QStringLiteral("lengthText"), QStringLiteral("1:00")},
            {QStringLiteral("path"), QStringLiteral("/music/%1.flac").arg(i)},
        });
    }
    return rows;
}

QString queuePath(int index) {
    return QStringLiteral("/music/%1.flac").arg(index);
}

// Plays queue row `index`, as the backend says it at once: its index, its
// path and the state. A gapless change sets them while playing.
void playQueueRow(QObject *bridge, int index, const QString &state = QStringLiteral("Playing")) {
    bridge->setProperty("currentTrackPath", queuePath(index));
    bridge->setProperty("playingQueueIndex", index);
    bridge->setProperty("playbackState", state);
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

const QString kArtistKey = QStringLiteral("artist|Artist A");
const QString kAlbumAKey = QStringLiteral("album|Artist A|Album A");
const QString kAlbumBKey = QStringLiteral("album|Artist A|Album B");
const QString kTrackA1 = QStringLiteral("/music/Artist A/Album A/01.flac");
const QString kTrackA2 = QStringLiteral("/music/Artist A/Album A/02.flac");
const QString kTrackB1 = QStringLiteral("/music/Artist A/Album B/01.flac");

// An artist with two albums: Album A of two tracks, Album B of one, in the
// bridge's tree format.
QByteArray libraryTree() {
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
        {1, 0, 0, 2, 0, QStringLiteral("Artist A (2)"), kArtistKey, QStringLiteral("/music/Artist A"), {}},
        {2, 1, 0, 2, 2005, QStringLiteral("Album A"), kAlbumAKey, QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 1, 0, 0, QStringLiteral("First"), QStringLiteral("track|") + kTrackA1, kTrackA1, kTrackA1},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackA2, kTrackA2, kTrackA2},
        {2, 1, 0, 1, 2007, QStringLiteral("Album B"), kAlbumBKey, QStringLiteral("/music/Artist A/Album B"), {}},
        {4, 2, 1, 0, 0, QStringLiteral("Third"), QStringLiteral("track|") + kTrackB1, kTrackB1, kTrackB1},
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

void showTree(LibraryTreeModel &model, const QStringList &expanded) {
    model.setExpandedKeys(expanded);
    model.setLibraryTreeFromBinary(libraryTree());
}

// Plays a library track, as the backend says it: the path and state at
// once, the rows that hold it a moment later with its metadata.
void playTreeTrack(QObject *bridge, const QString &path, const QString &albumKey) {
    bridge->setProperty("currentTrackPath", path);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("currentTrackTreeKeys", QStringList{kArtistKey, albumKey});
}

// The row of the library tree that shows `key`, as the view shows it.
QQuickItem *libraryRowWithKey(QQuickWindow *window, const QString &key) {
    QQuickItem *view = named(window, "libraryView");
    const int count = view != nullptr ? view->property("count").toInt() : 0;
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        if (row != nullptr && row->property("selectionKeyResolved").toString() == key) {
            return row;
        }
    }
    return nullptr;
}

QQuickItem *libraryRowWithPath(QQuickWindow *window, const QString &path) {
    QQuickItem *view = named(window, "libraryView");
    const int count = view != nullptr ? view->property("count").toInt() : 0;
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        if (row != nullptr && row->property("trackPathResolved").toString() == path) {
            return row;
        }
    }
    return nullptr;
}

// What a queue or tree row shows of its state: its live glyph, the glyph
// fading out, and its title.
struct RowLook {
    QQuickItem *glyph = nullptr;
    QQuickItem *fadingGlyph = nullptr;
    QQuickItem *title = nullptr;
};

RowLook queueRowLook(QQuickItem *row) {
    return {itemNamed(row, QStringLiteral("queueRowState")),
            itemNamed(row, QStringLiteral("queueRowFadingState")),
            itemNamed(row, QStringLiteral("queueCell_title"))};
}

RowLook libraryRowLook(QQuickItem *row) {
    return {itemNamed(row, QStringLiteral("libraryRowState")),
            itemNamed(row, QStringLiteral("libraryRowFadingState")),
            itemNamed(row, QStringLiteral("libraryRowTitle"))};
}

// Whether a row of a list shows its children's y as they are once its
// transitions are done: `y` of row `index` is `index` rows down.
bool rowsSettled(QQuickItem *view, int count) {
    const double rowHeight = view->property("rowHeight").toDouble();
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        if (row == nullptr || !qFuzzyCompare(row->y() + 1.0, index * rowHeight + 1.0)) {
            return false;
        }
    }
    return true;
}

// The frames `window` draws over `ms` of wall time.
int framesOver(QQuickWindow *window, int ms) {
    int frames = 0;
    const QMetaObject::Connection connection = QObject::connect(
        window, &QQuickWindow::frameSwapped, window, [&frames]() { ++frames; });
    QTest::qWait(ms);
    QObject::disconnect(connection);
    return frames;
}

} // namespace

class UiMotionTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void queueTrackChangeHandsTheAmberOver();
    void queueTrackChangeHandsTheAmberOver_data();
    void queueRepeatOneKeepsTheRowAmber();
    void queueEditsMovingThePlayingRowFadeNothing();
    void treeTrackChangeHandsTheAmberOver();
    void reusedTreeRowsShowNoStaleFade();
    void nowPlayingCrossFadesTheTextForTheNextTrackOfAnAlbum();
    void nowPlayingCrossFadesCoverAndTextForAnotherAlbum();
    void treeToggleOpensDownwardAndKeepsTheRowPut();
    void treeChangesOtherThanAToggleAreInstant();
    void queueRowsADragDisplacesSlide();
    void queueRowsTheKeyboardMovesMoveAtOnce();
    void animationsOffMakeEveryChangeInstant();
    void nothingAnimatesOnceTheMotionIsDone();
};

void UiMotionTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(captureMessage);
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
}

void UiMotionTest::init() {
    takeMessages();
}

void UiMotionTest::cleanup() {
    const QString messages = takeMessages();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiMotionTest::cleanupTestCase() {
    qInstallMessageHandler(previousHandler());
    previousHandler() = nullptr;
}

void UiMotionTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiMotionTest::queueTrackChangeHandsTheAmberOver_data() {
    QTest::addColumn<bool>("gapless");
    // A track the user starts: the state goes on playing either way.
    QTest::newRow("next") << false;
    // The next track starting at the end of this one, gaplessly.
    QTest::newRow("gapless") << true;
}

// The new playing row is amber at once; only the old row's amber glyph and
// title fade out, over shortDuration with OutCubic.
void UiMotionTest::queueTrackChangeHandsTheAmberOver() {
    QFETCH(bool, gapless);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 1);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 2) != nullptr; }));
    QQuickItem *pane = named(window, "mainQueuePane");
    QObject *fade = qvariant_cast<QObject *>(pane->property("nowFade"));
    QVERIFY(fade != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QVERIFY(units->shortDuration() > 0);
    QCOMPARE(animationOf(fade)->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(animationOf(fade)), QEasingCurve::OutCubic);

    const RowLook oldRow = queueRowLook(itemAtIndex(view, 1));
    const RowLook newRow = queueRowLook(itemAtIndex(view, 2));
    QCOMPARE(colorOf(oldRow.title), nowHex(window));
    QVERIFY(!oldRow.fadingGlyph->isVisible());

    if (gapless) {
        // The backend moves on in one step: index and path together.
        bridge->setProperty("playingQueueIndex", 2);
        bridge->setProperty("currentTrackPath", queuePath(2));
    } else {
        playQueueRow(bridge, 2);
    }

    // At once, before a frame: the new row is amber, the old one fades.
    QCOMPARE(colorOf(newRow.title), nowHex(window));
    QCOMPARE(newRow.title->property("font").value<QFont>().weight(), QFont::DemiBold);
    QVERIFY(newRow.glyph->isVisible());
    QCOMPARE(colorOf(newRow.glyph), nowHex(window));
    QVERIFY(!newRow.fadingGlyph->isVisible());
    QVERIFY(animationOf(fade)->property("running").toBool());
    QVERIFY(oldRow.fadingGlyph->isVisible());
    QCOMPARE(oldRow.fadingGlyph->property("source").toString(), QStringLiteral("media-playback-start"));
    QCOMPARE(colorOf(oldRow.fadingGlyph), nowHex(window));
    QCOMPARE(oldRow.fadingGlyph->opacity(), 1.0);
    QCOMPARE(colorOf(oldRow.title), nowHex(window));
    // Its weight goes with the state, at once.
    QCOMPARE(oldRow.title->property("font").value<QFont>().weight(), QFont::Normal);
    QVERIFY(!oldRow.glyph->isVisible());

    // Then it is plain, and nothing runs.
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    QVERIFY(!oldRow.fadingGlyph->isVisible());
    QVERIFY(colorOf(oldRow.title) != nowHex(window));
    QVERIFY(fade->property("target").isNull() || !fade->property("target").value<QJSValue>().isObject());
    QCOMPARE(colorOf(newRow.title), nowHex(window));
}

// Repeat-One plays the same row again: it stays amber, nothing fades.
void UiMotionTest::queueRepeatOneKeepsTheRowAmber() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    bridge->setProperty("repeatMode", 1);
    playQueueRow(bridge, 3);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 3) != nullptr; }));
    QObject *fade = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("nowFade"));

    // The backend says the same track again, as at the start of a repeat.
    playQueueRow(bridge, 3);
    const RowLook row = queueRowLook(itemAtIndex(view, 3));
    QCOMPARE(colorOf(row.title), nowHex(window));
    QVERIFY(row.glyph->isVisible());
    QVERIFY(!row.fadingGlyph->isVisible());
    QVERIFY(!animationOf(fade)->property("running").toBool());
}

// A queue edit that moves the playing row changes its index but not the
// track: no other row takes its amber, not even for a moment.
void UiMotionTest::queueEditsMovingThePlayingRowFadeNothing() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 3);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 4) != nullptr; }));

    // Row 1 is removed: the playing track is row 2 now, and row 3 is the
    // track that followed it.
    QVariantList rows = queueRows();
    rows.removeAt(1);
    bridge->setProperty("queueRows", rows);
    bridge->setProperty("queueLength", 4);
    bridge->setProperty("playingQueueIndex", 2);
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 3) != nullptr; }));
    for (int index = 0; index < 4; ++index) {
        QVERIFY2(!queueRowLook(itemAtIndex(view, index)).fadingGlyph->isVisible(),
                 qPrintable(QStringLiteral("row %1 fades").arg(index)));
    }
    QCOMPARE(colorOf(queueRowLook(itemAtIndex(view, 2)).title), nowHex(window));
}

// The tree hands the amber over as the queue does: the new track row at
// once, the old one fading; a collapsed album that held the old track
// fades its 60% glyph out.
void UiMotionTest::treeTrackChangeHandsTheAmberOver() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackA2) != nullptr; }));
    QQuickItem *pane = named(window, "libraryPane");
    QObject *fade = qvariant_cast<QObject *>(pane->property("nowFade"));
    QVERIFY(fade != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QCOMPARE(animationOf(fade)->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(animationOf(fade)), QEasingCurve::OutCubic);

    const RowLook first = libraryRowLook(libraryRowWithPath(window, kTrackA1));
    const RowLook second = libraryRowLook(libraryRowWithPath(window, kTrackA2));
    QCOMPARE(colorOf(first.title), nowHex(window));

    // The next track of the album: its row is amber at once. The backend
    // names the rows that hold it with its metadata, after its path.
    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTreeKeys", QStringList{});
    QCOMPARE(colorOf(second.title), nowHex(window));
    QVERIFY(second.glyph->isVisible());
    QVERIFY(!second.fadingGlyph->isVisible());
    QVERIFY(first.fadingGlyph->isVisible());
    QCOMPARE(first.fadingGlyph->opacity(), 1.0);
    QCOMPARE(colorOf(first.title), nowHex(window));
    bridge->setProperty("currentTrackTreeKeys", QStringList{kArtistKey, kAlbumAKey});
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    QVERIFY(!first.fadingGlyph->isVisible());
    QVERIFY(colorOf(first.title) != nowHex(window));

    // Album B is collapsed and holds the next track; then Album A plays
    // again, and Album B's dim mark fades out.
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    QVERIFY(albumB != nullptr);
    playTreeTrack(bridge, kTrackB1, kAlbumBKey);
    const RowLook album = libraryRowLook(albumB);
    QVERIFY(album.glyph->isVisible());
    QCOMPARE(album.glyph->opacity(), 0.6);
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(!album.glyph->isVisible());
    QVERIFY(album.fadingGlyph->isVisible());
    QCOMPARE(album.fadingGlyph->opacity(), 0.6);
    QCOMPARE(colorOf(album.fadingGlyph), nowHex(window));
    // Its title is never amber.
    QVERIFY(colorOf(album.title) != nowHex(window));
    QCOMPARE(colorOf(libraryRowLook(libraryRowWithPath(window, kTrackA1)).title), nowHex(window));
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    QVERIFY(!album.fadingGlyph->isVisible());
}

// The tree reuses its rows: a row that comes back for another track never
// shows the fade of the track it showed, nor a transition's leftovers.
void UiMotionTest::reusedTreeRowsShowNoStaleFade() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackA2) != nullptr; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    QObject *fade = qvariant_cast<QObject *>(named(window, "libraryPane")->property("nowFade"));

    // A1's amber fades; Album A closes while it does, and Album B opens in
    // its rows.
    playTreeTrack(bridge, kTrackB1, kAlbumBKey);
    QVERIFY(animationOf(fade)->property("running").toBool());
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumBKey));
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackB1) != nullptr; }));
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return rowsSettled(view, view->property("count").toInt()); }));
    for (int index = 0; index < view->property("count").toInt(); ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        QVERIFY(row != nullptr);
        QCOMPARE(row->opacity(), 1.0);
        QCOMPARE(row->z(), 1.0);
        const bool fadingA1 = row->property("trackPathResolved").toString() == kTrackA1;
        QVERIFY2(fadingA1 || !libraryRowLook(row).fadingGlyph->isVisible(),
                 qPrintable(QStringLiteral("row %1 shows a fade").arg(index)));
    }
    QCOMPARE(colorOf(libraryRowLook(libraryRowWithPath(window, kTrackB1)).title), nowHex(window));
}

// The next track of the same album: only the lines cross-fade, over
// shortDuration; the cover stays.
void UiMotionTest::nowPlayingCrossFadesTheTextForTheNextTrackOfAnAlbum() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("currentTrackPath", kTrackA1);
    bridge->setProperty("currentTrackTitle", QStringLiteral("First"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album A"));
    bridge->setProperty("currentTrackNumber", 1);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    QVERIFY(handover != nullptr);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(!itemNamed(panel, QStringLiteral("nowPlayingOutgoingWideText"))->isVisible());

    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackNumber", 2);

    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(handover), QEasingCurve::OutCubic);
    // The playing track's lines, coming in; the track before's going out.
    QCOMPARE(itemNamed(panel, QStringLiteral("nowPlayingTitle"))->property("text").toString(),
             QStringLiteral("Second"));
    QQuickItem *outgoing = itemNamed(panel, QStringLiteral("nowPlayingOutgoingWideText"));
    QVERIFY(outgoing->isVisible());
    QCOMPARE(itemNamed(panel, QStringLiteral("nowPlayingOutgoingTitle"))->property("text").toString(),
             QStringLiteral("First"));
    QCOMPARE(itemNamed(panel, QStringLiteral("nowPlayingOutgoingDetails"))->property("text").toString(),
             QStringLiteral("Track 1"));
    QVERIFY(!itemNamed(panel, QStringLiteral("albumArtOutgoing"))->isVisible());

    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(!itemNamed(panel, QStringLiteral("nowPlayingOutgoingWideText"))->isVisible());
    QCOMPARE(itemNamed(panel, QStringLiteral("nowPlayingWideText"))->parentItem()->opacity(), 1.0);

    // Metadata that comes later changes the lines in place.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    QVERIFY(!handover->property("running").toBool());
    QCOMPARE(itemNamed(panel, QStringLiteral("nowPlayingDetails"))->property("text").toString(),
             QStringLiteral("Classical · Track 2"));
}

// Another album: the cover and the lines cross-fade together, on one curve
// over longDuration.
void UiMotionTest::nowPlayingCrossFadesCoverAndTextForAnotherAlbum() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album A"));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));

    bridge->setProperty("currentTrackPath", kTrackB1);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Third"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album B"));

    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QCOMPARE(easingOf(handover), QEasingCurve::OutCubic);
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    QQuickItem *outgoingText = itemNamed(panel, QStringLiteral("nowPlayingOutgoingWideText"));
    QVERIFY(outgoingCover->isVisible());
    QVERIFY(outgoingText->isVisible());
    // One curve: the cover and the lines going out are at the same point.
    QCOMPARE(outgoingCover->opacity(), outgoingText->parentItem()->opacity());
    QVERIFY(QTest::qWaitFor([&]() {
        const double cover = outgoingCover->opacity();
        return cover < 0.9 && cover > 0.1;
    }));
    QCOMPARE(outgoingCover->opacity(), outgoingText->parentItem()->opacity());
    QCOMPARE(itemNamed(panel, QStringLiteral("nowPlayingOutgoingArtistAlbum"))->property("text").toString(),
             QStringLiteral("Album A"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(!outgoingCover->isVisible());
    QVERIFY(!outgoingText->isVisible());
}

// Expanding a row opens its children downward: the toggled row stays put,
// the rows below slide down from where they were over shortDuration with
// OutCubic. Collapsing slides them back up.
void UiMotionTest::treeToggleOpensDownwardAndKeepsTheRowPut() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return view->property("count").toInt() == 3; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    auto *open = qvariant_cast<QObject *>(view->property("openAnimation"));
    auto *close = qvariant_cast<QObject *>(view->property("closeAnimation"));
    auto *closeFade = qvariant_cast<QObject *>(view->property("closeFadeAnimation"));
    QVERIFY(open != nullptr && close != nullptr && closeFade != nullptr);
    for (QObject *animation : {open, close, closeFade}) {
        QCOMPARE(animation->property("duration").toInt(), units->shortDuration());
        QCOMPARE(easingOf(animation), QEasingCurve::OutCubic);
    }

    QQuickItem *albumA = libraryRowWithKey(window, kAlbumAKey);
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = view->property("rowHeight").toDouble();
    const double albumAY = albumA->y();
    QCOMPARE(albumB->y(), 2 * rowHeight);

    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    // Laid out at once: the children are in place, Album A has not moved,
    // and Album B starts where it was, over the children.
    QCOMPARE(view->property("count").toInt(), 5);
    QCOMPARE(albumA->y(), albumAY);
    QQuickItem *firstChild = libraryRowWithPath(window, kTrackA1);
    QVERIFY(firstChild != nullptr);
    QCOMPARE(firstChild->y(), 2 * rowHeight);
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QVERIFY(albumB->z() > firstChild->z());
    QVERIFY(QTest::qWaitFor([&]() {
        return albumB->y() > 2 * rowHeight && albumB->y() < 4 * rowHeight;
    }));
    QCOMPARE(albumA->y(), albumAY);
    QVERIFY(QTest::qWaitFor([&]() { return albumB->y() == 4 * rowHeight; }));
    QVERIFY(QTest::qWaitFor([&]() { return albumB->z() == 1.0; }));

    // Collapsing: Album B slides back up over the children as they fade.
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(view->property("count").toInt(), 3);
    QCOMPARE(albumA->y(), albumAY);
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QVERIFY(QTest::qWaitFor([&]() { return albumB->y() == 2 * rowHeight; }));
    QCOMPARE(albumA->y(), albumAY);
}

// Rows that change other than by a toggle, as when a scan or a restore
// brings them, move at once.
void UiMotionTest::treeChangesOtherThanAToggleAreInstant() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return view->property("count").toInt() == 3; }));
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = view->property("rowHeight").toDouble();

    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QVERIFY(QMetaObject::invokeMethod(view, "forceLayout"));
    QCOMPARE(view->property("count").toInt(), 5);
    QCOMPARE(albumB->y(), 4 * rowHeight);
}

// Rows a drag drops elsewhere: the rows they displace slide to their places
// over shortDuration with OutCubic, under the dropped row.
void UiMotionTest::queueRowsADragDisplacesSlide() {
    QueueRowsModel rows;
    QVector<QueueRowData> initial;
    for (int i = 0; i < 5; ++i) {
        QueueRowData data;
        data.title = QStringLiteral("Track %1").arg(i);
        data.path = queuePath(i);
        data.lengthText = QStringLiteral("1:00");
        initial.push_back(data);
    }
    rows.setRows(initial);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", QVariant::fromValue<QObject *>(&rows));
    bridge->setProperty("queueLength", 5);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 4) != nullptr; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("controller"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    auto *slide = qvariant_cast<QObject *>(view->property("slideAnimation"));
    QVERIFY(slide != nullptr);
    QCOMPARE(slide->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(slide), QEasingCurve::OutCubic);
    const double rowHeight = view->property("rowHeight").toDouble();

    // Row 0 dropped before row 3; the backend moves it there.
    QMetaObject::invokeMethod(controller, "dropQueueRows", Q_ARG(QVariant, 0), Q_ARG(QVariant, 3));
    QVERIFY(controller->property("movesSlide").toBool());
    QQuickItem *moved = itemAtIndex(view, 0);
    QQuickItem *displaced = itemAtIndex(view, 1);
    QVERIFY(rows.moveRow(0, 2));
    QVERIFY(QMetaObject::invokeMethod(view, "forceLayout"));
    // The dropped row is in place at once, over the row it displaces,
    // which starts where it was.
    QCOMPARE(moved->y(), 2 * rowHeight);
    QCOMPARE(displaced->y(), 1 * rowHeight);
    QVERIFY(moved->z() > displaced->z());
    QVERIFY(QTest::qWaitFor([&]() { return displaced->y() < rowHeight && displaced->y() > 0; }));
    QVERIFY(QTest::qWaitFor([&]() { return rowsSettled(view, 5); }));
    QCOMPARE(itemAtIndex(view, 2), moved);
}

// Moves by the keyboard are instant, so repeated moves never lag, also
// after a drag.
void UiMotionTest::queueRowsTheKeyboardMovesMoveAtOnce() {
    QueueRowsModel rows;
    QVector<QueueRowData> initial;
    for (int i = 0; i < 5; ++i) {
        QueueRowData data;
        data.title = QStringLiteral("Track %1").arg(i);
        data.path = queuePath(i);
        data.lengthText = QStringLiteral("1:00");
        initial.push_back(data);
    }
    rows.setRows(initial);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", QVariant::fromValue<QObject *>(&rows));
    bridge->setProperty("queueLength", 5);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 4) != nullptr; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("controller"));
    const double rowHeight = view->property("rowHeight").toDouble();

    QMetaObject::invokeMethod(controller, "dropQueueRows", Q_ARG(QVariant, 0), Q_ARG(QVariant, 3));
    QVERIFY(controller->property("movesSlide").toBool());
    QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 3));
    QMetaObject::invokeMethod(controller, "moveSelected", Q_ARG(QVariant, -1));
    QVERIFY(!controller->property("movesSlide").toBool());
    QQuickItem *displaced = itemAtIndex(view, 2);
    QVERIFY(rows.moveRow(3, 2));
    QVERIFY(QMetaObject::invokeMethod(view, "forceLayout"));
    QCOMPARE(displaced->y(), 3 * rowHeight);
    QVERIFY(rowsSettled(view, 5));
}

// With Plasma's animations off, Kirigami's durations are 0: every change
// above is instant, and nothing is left to fade.
void UiMotionTest::animationsOffMakeEveryChangeInstant() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setShortDuration(0);
    units->setLongDuration(0);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 1);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Track 1"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album"));
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 2) != nullptr; }));
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackA2) != nullptr; }));

    // The queue and the tree.
    playQueueRow(bridge, 2);
    playTreeTrack(bridge, kTrackA2, kAlbumAKey);
    const RowLook oldQueueRow = queueRowLook(itemAtIndex(view, 1));
    QVERIFY(!oldQueueRow.fadingGlyph->isVisible());
    QVERIFY(colorOf(oldQueueRow.title) != nowHex(window));
    QCOMPARE(colorOf(queueRowLook(itemAtIndex(view, 2)).title), nowHex(window));
    const RowLook oldTreeRow = libraryRowLook(libraryRowWithPath(window, kTrackA1));
    QVERIFY(!oldTreeRow.fadingGlyph->isVisible());
    QVERIFY(colorOf(oldTreeRow.title) != nowHex(window));

    // Now playing, for another album.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Third"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album B"));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    playQueueRow(bridge, 3);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(!itemNamed(panel, QStringLiteral("nowPlayingOutgoingWideText"))->isVisible());
    QVERIFY(!itemNamed(panel, QStringLiteral("albumArtOutgoing"))->isVisible());
    QCOMPARE(itemNamed(panel, QStringLiteral("nowPlayingWideText"))->parentItem()->opacity(), 1.0);

    // The tree's toggle.
    QQuickItem *tree = named(window, "libraryView");
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = tree->property("rowHeight").toDouble();
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QVERIFY(rowsSettled(tree, tree->property("count").toInt()));
}

// Once a change is done, nothing runs: no animation, no frame.
void UiMotionTest::nothingAnimatesOnceTheMotionIsDone() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 1);
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Track 1"));

    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 2) != nullptr; }));

    playQueueRow(bridge, 2);
    playTreeTrack(bridge, kTrackA2, kAlbumAKey);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Track 2"));
    QMetaObject::invokeMethod(qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller")),
                              "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QObject *queueFade = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("nowFade"));
    QObject *treeFade = qvariant_cast<QObject *>(named(window, "libraryPane")->property("nowFade"));
    QObject *handover = qvariant_cast<QObject *>(named(window, "nowPlayingPanel")->property("handoverAnimation"));
    QVERIFY(animationOf(queueFade)->property("running").toBool());
    QVERIFY(QTest::qWaitFor([&]() {
        return !animationOf(queueFade)->property("running").toBool()
            && !animationOf(treeFade)->property("running").toBool()
            && !handover->property("running").toBool()
            && rowsSettled(named(window, "libraryView"), 3);
    }));
    // The last frames of the motion, and of the scrollbar the style shows
    // as the tree's height changes, then none.
    QTest::qWait(1000);
    QCOMPARE(framesOver(window, 500), 0);
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
    // The same platform whatever the desktop the tests run in, and so
    // Kirigami's own durations, not a desktop's.
    qunsetenv("XDG_CURRENT_DESKTOP");
    qunsetenv("QT_QPA_PLATFORMTHEME");
    QApplication app(argc, argv);
    QIcon::setThemeName(QStringLiteral("breeze"));
    UiMotionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_motion.moc"
