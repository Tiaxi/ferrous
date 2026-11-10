// SPDX-License-Identifier: GPL-3.0-or-later

// The main window as one sheet (#373): the content on the view colour with
// four seams, the now-playing sidebar in its three layouts, the region
// headers of the library and the queue, the shared rows of the tree and the
// queue, the one-step library tree with its playing marks, the queue's
// columns, and the first run.

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJSValue>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlListReference>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <cmath>
#include <functional>

#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"
#include "../src/WindowPresence.h"
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
    // A window resize below the minimum size; the offscreen platform warns
    // that it does not pass size hints on.
    capturedMessages().removeAll(QStringLiteral("This plugin does not support propagateSizeHints()"));
    const QString text = capturedMessages().join(QLatin1Char('\n'));
    capturedMessages().clear();
    return text;
}

// Main.qml with its fallback bridge, `width` by `height`, shown and active.
QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel,
                             int width = 1600, int height = 1000) {
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

QString colorOf(const QObject *object) {
    return hex(object->property("color").value<QColor>());
}

QString tokenHex(QQuickWindow *window, const char *name) {
    return hex(tokenOf(window, name));
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

QQuickItem *itemNamed(QQuickItem *root, const QString &name) {
    const QList<QQuickItem *> found = itemsNamed(root, name);
    return found.isEmpty() ? nullptr : found.first();
}

// The first item named `name` in the window's item tree, which holds what
// repeaters and views make too.
QQuickItem *named(QQuickWindow *window, const char *name) {
    return itemNamed(window->contentItem(), QString::fromLatin1(name));
}

// Whether `item` and every item above it are visible.
bool shown(QQuickItem *item) {
    return item != nullptr && item->isVisible();
}

QRectF sceneRect(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// The first row of the library tree whose type is `rowType`, as the view
// shows it (not one of its pooled delegates).
QQuickItem *libraryRow(QQuickWindow *window, const QString &rowType) {
    QQuickItem *view = named(window, "libraryView");
    const int count = view != nullptr ? view->property("count").toInt() : 0;
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
        if (row != nullptr && row->property("rowTypeResolved").toString() == rowType) {
            return row;
        }
    }
    return nullptr;
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

// An artist with an album from 2005 of one track, number 2, in the bridge's
// tree format. The artist row's path is that of the folder of whichever of
// its tracks the backend met first.
QByteArray artistAlbumTrackTree(const QString &artistPath = QStringLiteral("/music/Artist A")) {
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
        {1, 0, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, artistPath, {}},
        {2, 1, 0, 1, 2005, QStringLiteral("Album A"), kAlbumKey,
         QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackPath, kTrackPath,
         kTrackPath},
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

void showTree(LibraryTreeModel &model, const QStringList &expanded,
              const QString &artistPath = QStringLiteral("/music/Artist A")) {
    model.setExpandedKeys(expanded);
    model.setLibraryTreeFromBinary(artistAlbumTrackTree(artistPath));
}

const QStringList kAllExpanded{kArtistKey, kAlbumKey};

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

void playTrack(QObject *bridge, const QString &state = QStringLiteral("Playing")) {
    bridge->setProperty("currentTrackPath", kTrackPath);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Morning Tide Field"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Small Hours"));
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    bridge->setProperty("currentTrackYear", 2005);
    bridge->setProperty("currentTrackNumber", 2);
    bridge->setProperty("currentTrackTotal", 8);
    // The rows that hold it, as the backend names them.
    bridge->setProperty("currentTrackTreeKeys", QStringList{QStringLiteral("root|/music"), kArtistKey, kAlbumKey});
    bridge->setProperty("playbackState", state);
}

} // namespace

class UiMainWindowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void contentIsOneSheetWithFourSeams();
    void sidebarIs360PxWideAnd260PxInANarrowWindow();
    void nowPlayingHasItsWideNarrowAndEmptyLayouts();
    void nowPlayingShowsTheFileNameOfATrackWithoutTitleOrArtist();
    void regionHeadersNameTheRegionAndItsTotals();
    void libraryTreeHasOneIndentStep();
    void libraryTreeMarksThePlayingTrackAndItsCollapsedAncestors();
    void collapsedArtistMarkFollowsTheBackendsGroupingNotItsPath();
    void libraryRowsShowHoverSelectionAndTheCurrentRow();
    void listsRingARowAsTheKeyboardComesIn();
    void unreadableTracksAreMarkedFromTheIssueHistory();
    void queueHeaderLabelsOnlyArtistAlbumAndLength();
    void narrowQueueDropsAlbumBeforeTitle();
    void firstRunShowsOneMessage();
    void viewersInTheWindowFadeAsTheStylesPopupsDo();
};

void UiMainWindowTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(captureMessage);
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
}

void UiMainWindowTest::init() {
    takeMessages();
}

void UiMainWindowTest::cleanup() {
    const QString messages = takeMessages();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiMainWindowTest::cleanupTestCase() {
    qInstallMessageHandler(previousHandler());
    previousHandler() = nullptr;
}

void UiMainWindowTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiMainWindowTest::contentIsOneSheetWithFourSeams() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *transport = named(window, "transportBar");
    QQuickItem *transportSeam = named(window, "transportSeam");
    QQuickItem *mainSplit = named(window, "mainSplit");
    QQuickItem *sidebar = named(window, "sidebarPane");
    QQuickItem *rightSplit = named(window, "rightSplit");
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *pane = named(window, "spectrogramPane");
    QQuickItem *statusSeam = named(window, "statusBarSeam");
    QVERIFY(transport && transportSeam && mainSplit && sidebar && rightSplit && queue && pane && statusSeam);

    // Content on the view colour, the visualization on its floor.
    QCOMPARE(colorOf(sidebar), tokenHex(window, "view"));
    QCOMPARE(colorOf(queue), tokenHex(window, "view"));
    QCOMPARE(colorOf(pane), QStringLiteral("#ff000000"));

    // Transport | content: a chrome line right under the transport, and
    // the content right under it.
    QCOMPARE(transportSeam->height(), 1.0);
    QCOMPARE(colorOf(transportSeam), tokenHex(window, "chromeLine"));
    QCOMPARE(sceneRect(transportSeam).top(), sceneRect(transport).bottom());
    QCOMPARE(sceneRect(mainSplit).top(), sceneRect(transportSeam).bottom());
    // Content | status bar: a chrome line on the status bar's top edge.
    QCOMPARE(statusSeam->height(), 1.0);
    QCOMPARE(colorOf(statusSeam), tokenHex(window, "chromeLine"));
    QVERIFY(std::abs(sceneRect(statusSeam).top() - sceneRect(mainSplit).bottom()) <= 0.5);

    // The two split handles are 1 px seams, with the panes on either side
    // and no gap.
    const QList<QQuickItem *> handles = itemsNamed(window->contentItem(), QStringLiteral("seamHandle"));
    QCOMPARE(handles.size(), 2);
    QQuickItem *columnSeam = handles.at(0)->width() == 1.0 ? handles.at(0) : handles.at(1);
    QQuickItem *queueSeam = columnSeam == handles.at(0) ? handles.at(1) : handles.at(0);
    QCOMPARE(columnSeam->width(), 1.0);
    QCOMPARE(queueSeam->height(), 1.0);
    for (QQuickItem *seam : {columnSeam, queueSeam}) {
        QCOMPARE(colorOf(seam), tokenHex(window, "seam"));
    }
    QCOMPARE(sceneRect(columnSeam).left(), sceneRect(sidebar).right());
    QCOMPARE(sceneRect(rightSplit).left(), sceneRect(columnSeam).right());
    QCOMPARE(sceneRect(queueSeam).top(), sceneRect(queue).bottom());
    QCOMPARE(sceneRect(pane).top(), sceneRect(queueSeam).bottom());

    // A seam can be grabbed a few pixels either side of its line.
    const QRectF columnRect = sceneRect(columnSeam);
    QVERIFY(columnSeam->contains(columnSeam->mapFromScene(QPointF(columnRect.left() - 2, 300))));
    QVERIFY(columnSeam->contains(columnSeam->mapFromScene(QPointF(columnRect.left() + 3, 300))));
}

void UiMainWindowTest::sidebarIs360PxWideAnd260PxInANarrowWindow() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *sidebar = named(window, "sidebarPane");
    QVERIFY(sidebar != nullptr);
    QTRY_COMPARE(sidebar->width(), 360.0);
    window->resize(1000, 720);
    QTRY_COMPARE(window->width(), 1000);
    QTRY_COMPARE(sidebar->width(), 260.0);
}

void UiMainWindowTest::nowPlayingHasItsWideNarrowAndEmptyLayouts() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *palette = paletteOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *emptyRow = named(window, "nowPlayingEmptyRow");
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(panel && cover && emptyRow && header);
    const qreal rowHeight = palette->property("rowHeight").toReal();

    // Nothing loaded: one row, the glyph at the text inset and "Nothing
    // playing" in the secondary colour; no placeholder cover.
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("empty"));
    QVERIFY(shown(emptyRow));
    QVERIFY(!cover->isVisible());
    QCOMPARE(emptyRow->height(), rowHeight);
    QVERIFY(panel->height() < 3 * rowHeight);
    QQuickItem *emptyLabel = itemNamed(emptyRow, QStringLiteral("nowPlayingEmptyLabel"));
    QVERIFY(emptyLabel != nullptr);
    QCOMPARE(emptyLabel->property("text").toString(), QStringLiteral("Nothing playing"));
    QCOMPARE(colorOf(emptyLabel), tokenHex(window, "textSecondary"));
    QCOMPARE(sceneRect(header).top(), sceneRect(panel).bottom());

    // Playing, wide: the cover edge to edge and square, then the text 12 px
    // in and 12 px under it, with no card around it.
    playTrack(bridge);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QTRY_COMPARE(cover->width(), panel->width());
    QCOMPARE(cover->height(), cover->width());
    QCOMPARE(sceneRect(cover).left(), sceneRect(panel).left());
    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *artistAlbum = named(window, "nowPlayingArtistAlbum");
    QQuickItem *details = named(window, "nowPlayingDetails");
    QVERIFY(title && artistAlbum && details);
    QCOMPARE(sceneRect(title).left() - sceneRect(panel).left(), 12.0);
    QCOMPARE(sceneRect(title).top() - sceneRect(cover).bottom(), 12.0);
    QCOMPARE(title->property("text").toString(), QStringLiteral("Morning Tide Field"));
    QCOMPARE(artistAlbum->property("text").toString(), QStringLiteral("The Quiet Meridian — Small Hours"));
    QCOMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2 of 8"));
    // Without a total the backend is sure of, the number alone.
    bridge->setProperty("currentTrackTotal", 0);
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2"));
    bridge->setProperty("currentTrackTotal", 8);
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2 of 8"));
    const QFont titleFont = title->property("font").value<QFont>();
    QCOMPARE(titleFont.weight(), QFont::DemiBold);
    QCOMPARE(titleFont.pointSizeF(), palette->property("basePointSize").toReal() * 15 / 13);
    QCOMPARE(colorOf(title), tokenHex(window, "text"));
    QCOMPARE(colorOf(artistAlbum), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(details), tokenHex(window, "textTertiary"));
    QCOMPARE(details->property("font").value<QFont>().pointSizeF(),
             palette->property("smallPointSize").toReal());
    QVERIFY(!emptyRow->isVisible());
    QTRY_VERIFY(sceneRect(header).top() >= sceneRect(details).bottom());

    // Missing values are left out, not shown as dashes.
    bridge->setProperty("currentTrackGenre", QString());
    bridge->setProperty("currentTrackYear", QVariant());
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("Track 2 of 8"));

    // A title that does not fit is elided, with the whole of it in the
    // tooltip; nothing scrolls.
    const QString longTitle = QStringLiteral("A Remarkably Long Track Title That Will Certainly Need Eliding");
    bridge->setProperty("currentTrackTitle", longTitle);
    QTRY_VERIFY(title->property("truncated").toBool());
    QVERIFY(panel->property("textTruncated").toBool());
    QCOMPARE(title->property("x").toReal(), 0.0);

    // Narrow: a 96 px cover with the lines beside it, then 12 px before the
    // library.
    window->resize(1000, 720);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QTRY_COMPARE(cover->width(), 96.0);
    QCOMPARE(cover->height(), 96.0);
    QQuickItem *narrowTitle = named(window, "nowPlayingNarrowTitle");
    QVERIFY(shown(narrowTitle));
    QCOMPARE(sceneRect(narrowTitle).left() - sceneRect(cover).right(), 12.0);
    QCOMPARE(narrowTitle->property("font").value<QFont>().pointSizeF(),
             palette->property("basePointSize").toReal() * 14 / 13);
    QTRY_COMPARE(sceneRect(header).top() - sceneRect(cover).bottom(), 12.0);

    // The narrow lines have the year with the album and the total with the
    // number, or the number alone without a total.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    bridge->setProperty("currentTrackYear", 2005);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(),
                 QStringLiteral("Classical · Track 2 of 8"));
    QCOMPARE(panel->property("narrowAlbumText").toString(), QStringLiteral("Small Hours (2005)"));
    bridge->setProperty("currentTrackTotal", 0);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · Track 2"));
    // Nor is a total below the number shown.
    bridge->setProperty("currentTrackTotal", 1);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · Track 2"));
}

// A track whose tag lacks its title and artist shows its file name as the
// title and its album alone, with no stand-in for the artist (#560).
void UiMainWindowTest::nowPlayingShowsTheFileNameOfATrackWithoutTitleOrArtist() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    playTrack(bridge);
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/Album A/untitled take.flac"));
    bridge->setProperty("currentTrackTitle", QString());
    bridge->setProperty("currentTrackArtist", QString());
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QVERIFY(panel != nullptr);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *artistAlbum = named(window, "nowPlayingArtistAlbum");
    QVERIFY(title && artistAlbum);
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("untitled take.flac"));
    QTRY_COMPARE(artistAlbum->property("text").toString(), QStringLiteral("Small Hours"));
}

void UiMainWindowTest::regionHeadersNameTheRegionAndItsTotals() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *libraryHeader = named(window, "libraryHeader");
    QQuickItem *queueHeader = named(window, "queueHeader");
    QVERIFY(libraryHeader && queueHeader);
    QQuickItem *libraryTitle = itemNamed(libraryHeader, QStringLiteral("regionHeaderTitle"));
    QQuickItem *libraryTotals = itemNamed(libraryHeader, QStringLiteral("regionHeaderTotals"));
    QQuickItem *queueTitle = itemNamed(queueHeader, QStringLiteral("regionHeaderTitle"));
    QQuickItem *queueTotals = itemNamed(queueHeader, QStringLiteral("regionHeaderTotals"));
    QVERIFY(libraryTitle && libraryTotals && queueTitle && queueTotals);

    bridge->setProperty("libraryRootCount", 1);
    bridge->setProperty("libraryArtistCount", 12);
    bridge->setProperty("libraryAlbumCount", 28);
    bridge->setProperty("libraryTrackCount", 176);
    QTRY_COMPARE(libraryTotals->property("text").toString(),
                 QStringLiteral("12 artists · 28 albums · 176 tracks"));
    QCOMPARE(libraryTitle->property("text").toString(), QStringLiteral("Library"));
    QVERIFY(!libraryTitle->property("truncated").toBool());
    QCOMPARE(colorOf(libraryTotals), tokenHex(window, "textTertiary"));
    bridge->setProperty("libraryArtistCount", 1);
    QTRY_COMPARE(libraryTotals->property("text").toString(),
                 QStringLiteral("1 artist · 28 albums · 176 tracks"));
    // A scan shows its progress in the status bar, not after the totals.
    QQuickItem *scanCount = named(window, "statusBarScanCount");
    QVERIFY(scanCount != nullptr);
    // The status bar shows the scan of a connected bridge.
    bridge->setProperty("connected", true);
    bridge->setProperty("libraryScanProcessed", 41);
    bridge->setProperty("libraryScanDiscovered", 41);
    bridge->setProperty("libraryScanTotal", 120);
    bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(shown(scanCount));
    QCOMPARE(scanCount->property("text").toString(), QStringLiteral("41 / 120"));
    QCOMPARE(libraryTotals->property("text").toString(),
             QStringLiteral("1 artist · 28 albums · 176 tracks"));
    bridge->setProperty("libraryScanInProgress", false);
    QTRY_VERIFY(!shown(scanCount));

    // The queue's name and totals.
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    bridge->setProperty("queueDurationText", QStringLiteral("3:00"));
    QTRY_COMPARE(queueTotals->property("text").toString(), QStringLiteral("3 tracks · 3:00"));
    QCOMPARE(queueTitle->property("text").toString(), QStringLiteral("Queue"));

    // Narrow: the library's totals keep the tracks only.
    window->resize(1000, 720);
    QTRY_COMPARE(libraryTotals->property("text").toString(), QStringLiteral("176 tracks"));
    QVERIFY(!libraryTitle->property("truncated").toBool());
}

void UiMainWindowTest::libraryTreeHasOneIndentStep() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(libraryModel, kAllExpanded);
    QQuickItem *artist = nullptr;
    QQuickItem *album = nullptr;
    QQuickItem *track = nullptr;
    QTRY_VERIFY((artist = libraryRow(window, QStringLiteral("artist"))) != nullptr
                && (album = libraryRow(window, QStringLiteral("album"))) != nullptr
                && (track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    const qreal rowHeight = paletteOf(window)->property("rowHeight").toReal();
    for (QQuickItem *row : {artist, album, track}) {
        QCOMPARE(row->height(), rowHeight);
    }
    // The x of `name` in `row`, from the row's left edge.
    const auto xOf = [](QQuickItem *row, const char *name) {
        QQuickItem *item = itemNamed(row, QString::fromLatin1(name));
        return shown(item) ? item->mapToItem(row, QPointF()).x() : -1.0;
    };
    const auto text = [](QQuickItem *row, const char *name) {
        QQuickItem *item = itemNamed(row, QString::fromLatin1(name));
        return item != nullptr ? item->property("text").toString() : QString();
    };

    // Artist: the chevron at 12, its name 6 px after; no album count.
    QCOMPARE(xOf(artist, "libraryRowChevron"), 12.0);
    QCOMPARE(xOf(artist, "libraryRowTitle"), 34.0);
    QCOMPARE(text(artist, "libraryRowTitle"), QStringLiteral("Artist A"));
    QCOMPARE(itemNamed(artist, QStringLiteral("libraryRowChevron"))->property("source").toString(),
             QStringLiteral("arrow-down"));
    // Album: one step in, then the 20 px thumbnail, then the name at 78;
    // the year at the right end, tertiary, clear of the scrollbar lane.
    QCOMPARE(xOf(album, "libraryRowChevron"), 28.0);
    QCOMPARE(xOf(album, "libraryRowThumbnail"), 50.0);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowThumbnail"))->width(), 20.0);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowThumbnail"))->property("radius").toReal(), 2.0);
    QCOMPARE(xOf(album, "libraryRowTitle"), 78.0);
    QCOMPARE(text(album, "libraryRowTitle"), QStringLiteral("Album A"));
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(shown(year));
    QCOMPARE(year->property("text").toString(), QStringLiteral("2005"));
    QCOMPARE(year->x() + year->width(), album->width() - 8 - 12);
    QCOMPARE(colorOf(year), tokenHex(window, "textTertiary"));
    // Track: its number under the thumbnail, right-aligned, and its title
    // where the album's name starts; no number in the title.
    QQuickItem *number = itemNamed(track, QStringLiteral("libraryRowNumber"));
    QVERIFY(shown(number));
    QCOMPARE(xOf(track, "libraryRowNumber"), 50.0);
    QCOMPARE(number->width(), 20.0);
    QCOMPARE(number->property("horizontalAlignment").toInt(), int(Qt::AlignRight));
    QCOMPARE(number->property("text").toString(), QStringLiteral("2"));
    QCOMPARE(colorOf(number), tokenHex(window, "textTertiary"));
    QCOMPARE(xOf(track, "libraryRowTitle"), 78.0);
    QCOMPARE(text(track, "libraryRowTitle"), QStringLiteral("Second"));
    QVERIFY(!shown(itemNamed(track, QStringLiteral("libraryRowChevron"))));
    QVERIFY(!shown(itemNamed(track, QStringLiteral("libraryRowYear"))));

    // A collapsed album points right.
    libraryModel.toggleKey(QStringLiteral("album|Artist A|Album A"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    album = libraryRow(window, QStringLiteral("album"));
    QVERIFY(album != nullptr);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowChevron"))->property("source").toString(),
             QStringLiteral("arrow-right"));
}

void UiMainWindowTest::libraryTreeMarksThePlayingTrackAndItsCollapsedAncestors() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    const auto glyph = [](QQuickItem *row) { return itemNamed(row, QStringLiteral("libraryRowState")); };
    const auto glyphName = [&glyph](QQuickItem *row) {
        QQuickItem *item = row != nullptr ? glyph(row) : nullptr;
        return shown(item) ? item->property("source").toString() : QString();
    };
    const auto title = [](QQuickItem *row) { return itemNamed(row, QStringLiteral("libraryRowTitle")); };

    // Playing: an amber glyph in the album's chevron column and a semibold
    // amber title.
    playTrack(bridge);
    QQuickItem *track = libraryRow(window, QStringLiteral("track"));
    QTRY_COMPARE(glyphName(track), QStringLiteral("media-playback-start"));
    QCOMPARE(colorOf(glyph(track)), tokenHex(window, "now"));
    QCOMPARE(glyph(track)->width(), 12.0);
    QCOMPARE(glyph(track)->mapToItem(track, QPointF()).x(), 28.0 + 2.0);
    QCOMPARE(colorOf(title(track)), tokenHex(window, "now"));
    QCOMPARE(title(track)->property("font").value<QFont>().weight(), QFont::DemiBold);
    // Its expanded album and artist show nothing.
    QCOMPARE(glyphName(libraryRow(window, QStringLiteral("album"))), QString());
    QCOMPARE(glyphName(libraryRow(window, QStringLiteral("artist"))), QString());
    // Paused: the pause glyph, still amber.
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(glyphName(track), QStringLiteral("media-playback-pause"));
    QCOMPARE(colorOf(title(track)), tokenHex(window, "now"));
    // Stopped: a neutral stop mark, and the title as any other.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(glyphName(track), QStringLiteral("media-playback-stop"));
    QCOMPARE(colorOf(glyph(track)), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(title(track)), tokenHex(window, "text"));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));

    // A collapsed album that holds the playing track: the amber glyph at
    // 60% in the column left of its chevron. Its title stays un-amber.
    libraryModel.toggleKey(QStringLiteral("album|Artist A|Album A"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    QQuickItem *album = libraryRow(window, QStringLiteral("album"));
    QVERIFY(album != nullptr);
    QTRY_COMPARE(glyphName(album), QStringLiteral("media-playback-start"));
    QCOMPARE(colorOf(glyph(album)), tokenHex(window, "now"));
    QCOMPARE(glyph(album)->opacity(), 0.6);
    QCOMPARE(glyph(album)->mapToItem(album, QPointF()).x(), 12.0 + 2.0);
    QCOMPARE(colorOf(title(album)), tokenHex(window, "text"));
    QCOMPARE(title(album)->property("font").value<QFont>().weight(), QFont::Normal);
    // So does a collapsed artist.
    libraryModel.collapseAll();
    QTRY_VERIFY(libraryRow(window, QStringLiteral("album")) == nullptr);
    QQuickItem *artist = libraryRow(window, QStringLiteral("artist"));
    QVERIFY(artist != nullptr);
    QTRY_COMPARE(glyphName(artist), QStringLiteral("media-playback-start"));
    QCOMPARE(glyph(artist)->opacity(), 0.6);
    // Paused, the pause glyph; stopped, nothing: amber means now.
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(glyphName(artist), QStringLiteral("media-playback-pause"));
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(glyphName(artist), QString());
    // Another track, in other rows: nothing.
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist B/Album/01.flac"));
    bridge->setProperty("currentTrackTreeKeys", QStringList{QStringLiteral("artist|Artist B")});
    QTRY_COMPARE(glyphName(artist), QString());
}

void UiMainWindowTest::collapsedArtistMarkFollowsTheBackendsGroupingNotItsPath() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    const auto artistMark = [&]() {
        QQuickItem *artist = libraryRow(window, QStringLiteral("artist"));
        QQuickItem *glyph = artist != nullptr ? itemNamed(artist, QStringLiteral("libraryRowState")) : nullptr;
        return shown(glyph) ? glyph->property("source").toString() : QString();
    };
    // One artist gathers a loose file right in the library folder, tagged
    // with its name, and an album track in its own folder. Its row has the
    // path of whichever the backend met first, which a rescan can change.
    for (const QString &artistPath : {QStringLiteral("/music"), QStringLiteral("/music/Artist A")}) {
        showTree(libraryModel, {}, artistPath);
        QTRY_VERIFY(libraryRow(window, QStringLiteral("artist")) != nullptr);
        QCOMPARE(libraryRow(window, QStringLiteral("artist"))->property("openPathResolved").toString(),
                 artistPath);
        // The loose file.
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/single.flac"));
        bridge->setProperty("currentTrackTreeKeys", QStringList{QStringLiteral("root|/music"), kArtistKey});
        QTRY_COMPARE(artistMark(), QStringLiteral("media-playback-start"));
        // The album track.
        bridge->setProperty("currentTrackPath", kTrackPath);
        bridge->setProperty("currentTrackTreeKeys",
                            QStringList{QStringLiteral("root|/music"), kArtistKey, kAlbumKey});
        QTRY_COMPARE(artistMark(), QStringLiteral("media-playback-start"));
        // A track the library does not have: no rows hold it.
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/elsewhere.flac"));
        bridge->setProperty("currentTrackTreeKeys", QStringList{});
        QTRY_COMPARE(artistMark(), QString());
    }
}

void UiMainWindowTest::libraryRowsShowHoverSelectionAndTheCurrentRow() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(libraryModel, kAllExpanded);
    QQuickItem *album = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(view && controller);
    QQuickItem *tint = itemNamed(album, QStringLiteral("rowBackgroundTint"));
    QQuickItem *hover = itemNamed(album, QStringLiteral("rowBackgroundHover"));
    QVERIFY(tint && hover);
    // The rows leave the 8 px scrollbar lane free; the scrollbar shows only
    // when the tree is longer than the view.
    QCOMPARE(tint->width(), album->width() - 8);
    QCOMPARE(tint->mapToItem(album, QPointF()).x(), 0.0);
    QQuickItem *scrollBar = named(window, "libraryScrollBar");
    QVERIFY(scrollBar != nullptr);
    QVERIFY(!scrollBar->isVisible());
    QCOMPARE(scrollBar->width(), 8.0);

    // At rest: neither tint.
    QVERIFY(!tint->isVisible());
    QVERIFY(!hover->isVisible());
    // Hover: the hover tint.
    QTest::mouseMove(window, sceneRect(album).center().toPoint());
    QTRY_VERIFY(hover->isVisible() && hover->opacity() == 1.0);
    QCOMPARE(colorOf(hover), tokenHex(window, "hover"));
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 60));
    QTRY_VERIFY(!hover->isVisible());

    // Selected without focus: the inactive tint, no ring; the year steps up
    // to the secondary colour.
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 1)));
    named(window, "playlistView")->forceActiveFocus();
    QTRY_VERIFY(!view->hasActiveFocus());
    QTRY_VERIFY(tint->isVisible());
    QCOMPARE(colorOf(tint), tokenHex(window, "selectionInactive"));
    QCOMPARE(QQmlProperty(tint, QStringLiteral("border.width")).read().toReal(), 0.0);
    QCOMPARE(colorOf(year), tokenHex(window, "textSecondary"));
    // With focus: the tint, and the ring on the current row as one
    // rectangle.
    view->forceActiveFocus();
    QTRY_VERIFY(view->hasActiveFocus());
    QTRY_COMPARE(colorOf(tint), tokenHex(window, "selection"));
    QCOMPARE(QQmlProperty(tint, QStringLiteral("border.width")).read().toReal(), 1.0);
    QCOMPARE(hex(QQmlProperty(tint, QStringLiteral("border.color")).read().value<QColor>()),
             tokenHex(window, "focusRing"));
    // Down moves the current row, and the ring with it.
    QTest::keyClick(window, Qt::Key_Down);
    QQuickItem *track = libraryRow(window, QStringLiteral("track"));
    QQuickItem *trackTint = itemNamed(track, QStringLiteral("rowBackgroundTint"));
    QTRY_COMPARE(QQmlProperty(trackTint, QStringLiteral("border.width")).read().toReal(), 1.0);
    QTRY_VERIFY(!tint->isVisible());
    QCOMPARE(colorOf(itemNamed(track, QStringLiteral("libraryRowNumber"))), tokenHex(window, "textSecondary"));
}

// Tab into the library or the queue with no row selected: a row it shows
// becomes the current one and has the 1 px ring, so the keyboard's place
// shows. Nothing is selected and the view stays where it is.
void UiMainWindowTest::listsRingARowAsTheKeyboardComesIn() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    QQuickItem *libraryView = named(window, "libraryView");
    QQuickItem *queueView = named(window, "playlistView");
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QObject *queue = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(libraryView && queueView && library && queue);
    const auto rowAt = [](QQuickItem *view, int index) {
        QQuickItem *row = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
        return row;
    };
    // Whether row `index` of `view` has the ring alone: no selection tint.
    const auto ringedAlone = [&rowAt](QQuickItem *view, int index) {
        QQuickItem *row = rowAt(view, index);
        QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
        return shown(tint) && QQmlProperty(tint, QStringLiteral("border.width")).read().toReal() == 1.0
            && tint->property("color").value<QColor>().alpha() == 0;
    };
    const auto ringedRows = [&rowAt](QQuickItem *view) {
        QList<int> rows;
        for (int index = 0; index < view->property("count").toInt(); ++index) {
            QQuickItem *row = rowAt(view, index);
            QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
            if (shown(tint) && QQmlProperty(tint, QStringLiteral("border.width")).read().toReal() > 0) {
                rows.push_back(index);
            }
        }
        return rows;
    };
    const auto tabInto = [window](QQuickItem *view) {
        named(window, "transportButton_playPause")->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(!view->hasActiveFocus());
        view->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(view->hasActiveFocus());
    };

    // The library: its first row, with nothing selected.
    tabInto(libraryView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{0});
    QVERIFY(ringedAlone(libraryView, 0));
    QCOMPARE(library->property("selectedSelectionKey").toString(), QString());
    QCOMPARE(libraryModel.selectedKeys(), QStringList());
    // The keys go on from it: Down selects the next row.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(library->property("selectedSelectionKey").toString(),
                 rowAt(libraryView, 1)->property("selectionKeyResolved").toString());
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{1});
    QVERIFY(!ringedAlone(libraryView, 1));
    // Deselected, the row stays the current one.
    QVERIFY(QMetaObject::invokeMethod(library, "clearPrimarySelection"));
    libraryModel.clearSelection();
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{1});
    QVERIFY(ringedAlone(libraryView, 1));

    // Playing, a fresh tree rings the playing track's row,
    playTrack(bridge);
    library->setProperty("focusSelectionKey", QString());
    tabInto(libraryView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{2});
    QVERIFY(ringedAlone(libraryView, 2));
    // or the collapsed row that holds it.
    library->setProperty("focusSelectionKey", QString());
    libraryModel.toggleKey(kAlbumKey);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    tabInto(libraryView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{1});
    QCOMPARE(libraryModel.selectedKeys(), QStringList());
    // Without the keyboard, no ring.
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{});

    // The queue, scrolled down: its first row shown, and it stays put.
    bridge->setProperty("queueRows", sampleQueueRows(80));
    bridge->setProperty("queueLength", 80);
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(queueView->property("count").toInt(), 80);
    const qreal rowHeight = queueView->property("rowHeight").toReal();
    QVERIFY(rowHeight > 0);
    named(window, "transportButton_playPause")->forceActiveFocus(Qt::TabFocusReason);
    queueView->setProperty("contentY", 10 * rowHeight);
    QTRY_VERIFY(rowAt(queueView, 10) != nullptr);
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(queueView), QList<int>{10});
    QVERIFY(ringedAlone(queueView, 10));
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    QCOMPARE(queue->property("selectedIndices").toList().size(), 0);
    // Enter plays the ringed row.
    QJSValue played = engine.newObject();
    queue->setProperty("playAt", QVariant::fromValue(
        engine.evaluate(QStringLiteral("(function(played) { return function(index) { played.index = index } })"))
            .call({played})));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(played.property(QStringLiteral("index")).toInt(), 10);
    // The playing row, when it shows,
    bridge->setProperty("playingQueueIndex", 14);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    queue->setProperty("focusRowIndex", -1);
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(queueView), QList<int>{14});
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    // but not one off the view: the queue does not move to it.
    bridge->setProperty("playingQueueIndex", 2);
    queue->setProperty("focusRowIndex", -1);
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(queueView), QList<int>{10});
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    QCOMPARE(queue->property("selectedIndices").toList().size(), 0);
    // Down goes on from it, selecting the next row.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(queue->property("selectedIndices").toList(), QVariantList{11});
    QTRY_COMPARE(ringedRows(queueView), QList<int>{11});
}

void UiMainWindowTest::unreadableTracksAreMarkedFromTheIssueHistory() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    showTree(libraryModel, kAllExpanded);
    QVariantList rows = sampleQueueRows(2);
    QVariantMap second = rows.at(1).toMap();
    second.insert(QStringLiteral("path"), kTrackPath);
    rows[1] = second;
    bridge->setProperty("queueRows", rows);
    bridge->setProperty("queueLength", 2);
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    const auto unreadablePaths = [errors]() { return errors->property("unreadablePaths").toMap(); };

    // A warning, or an error about another kind of thing, marks nothing.
    // Category 1 is playback, 2 the library; severity 0 a warning, 1 an error.
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, QStringLiteral("Slow"))));
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 2), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, QStringLiteral("Scan"))));
    QVERIFY(unreadablePaths().isEmpty());
    // A playback error about the file marks its rows: a neutral warning
    // glyph, and the title in the secondary colour.
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath),
                                      Q_ARG(QVariant, QStringLiteral("The file cannot be read."))));
    QCOMPARE(unreadablePaths().value(kTrackPath).toString(), QStringLiteral("The file cannot be read."));
    QQuickItem *treeGlyph = itemNamed(track, QStringLiteral("libraryRowState"));
    QTRY_VERIFY(shown(treeGlyph));
    QCOMPARE(treeGlyph->property("source").toString(), QStringLiteral("dialog-warning"));
    QCOMPARE(colorOf(treeGlyph), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(itemNamed(track, QStringLiteral("libraryRowTitle"))), tokenHex(window, "textSecondary"));
    QCOMPARE(QQmlProperty(track, QStringLiteral("Accessible.description"), qmlContext(track)).read().toString(),
             QStringLiteral("Track, The file cannot be read."));
    QQuickItem *queueRow = nullptr;
    QTRY_VERIFY(([&]() {
        for (QQuickItem *row : itemsNamed(named(window, "playlistView"), QStringLiteral("queueRow"))) {
            if (row->property("queueRowIndex").toInt() == 1) {
                queueRow = row;
            }
        }
        return queueRow != nullptr;
    })());
    QQuickItem *queueGlyph = itemNamed(queueRow, QStringLiteral("queueRowState"));
    QTRY_VERIFY(shown(queueGlyph));
    QCOMPARE(queueGlyph->property("source").toString(), QStringLiteral("dialog-warning"));
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_title"))), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_artist"))), tokenHex(window, "textSecondary"));
    QVERIFY(QQmlProperty(queueRow, QStringLiteral("Accessible.name"), qmlContext(queueRow))
                .read().toString().endsWith(QStringLiteral(", The file cannot be read.")));
    // Clearing the history clears the marks.
    QVERIFY(QMetaObject::invokeMethod(errors, "clear"));
    QTRY_VERIFY(!treeGlyph->isVisible());
    QTRY_VERIFY(!queueGlyph->isVisible());
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_artist"))), tokenHex(window, "textColumn"));
}

void UiMainWindowTest::queueHeaderLabelsOnlyArtistAlbumAndLength() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *palette = paletteOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QQuickItem *queueHeader = named(window, "queueHeader");
    QVERIFY(queueHeader != nullptr);
    QCOMPARE(queueHeader->height(), palette->property("rowHeight").toReal());

    // The region's name sits where the Title label would be.
    QQuickItem *title = itemNamed(queueHeader, QStringLiteral("queueHeader_title"));
    QQuickItem *name = itemNamed(queueHeader, QStringLiteral("regionHeaderTitle"));
    QVERIFY(title && name);
    QTRY_COMPARE(name->mapToScene(QPointF()).x(), title->mapToScene(QPointF()).x());
    // The glyph and number columns have no label.
    for (const char *column : {"queueHeader_indicator", "queueHeader_order", "queueHeader_title"}) {
        QQuickItem *header = itemNamed(queueHeader, QString::fromLatin1(column));
        QVERIFY(header != nullptr);
        QVERIFY2(!header->property("text").isValid(), column);
        QVERIFY2(header->childItems().isEmpty(), column);
    }
    // Artist, Album and Length: small labels in the secondary colour, Length
    // right-aligned 24 px (at the default font) from the queue's right edge.
    QQuickItem *queue = named(window, "mainQueuePane");
    for (const char *column : {"queueHeader_artist", "queueHeader_album", "queueHeader_length"}) {
        QQuickItem *label = itemNamed(queueHeader, QString::fromLatin1(column));
        QVERIFY2(shown(label), column);
        QCOMPARE(colorOf(label), tokenHex(window, "textSecondary"));
        QCOMPARE(label->property("font").value<QFont>().pointSizeF(),
                 palette->property("smallPointSize").toReal());
    }
    QQuickItem *length = itemNamed(queueHeader, QStringLiteral("queueHeader_length"));
    QCOMPARE(length->property("horizontalAlignment").toInt(), int(Qt::AlignRight));
    QCOMPARE(sceneRect(queue).right() - sceneRect(length).right(), queue->property("rightPadding").toReal());
    QCOMPARE(queue->property("rightPadding").toReal(),
             std::round(24 * palette->property("rowHeight").toReal() / 26));
    // The resize handles show no line until the pointer or the keyboard
    // finds them.
    QQuickItem *handle = named(window, "queueColumnHandle_album");
    QVERIFY(shown(handle));
    QQuickItem *line = itemNamed(handle, QStringLiteral("queueColumnHandleLine"));
    QVERIFY(line != nullptr && !line->isVisible());
    handle->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(line->isVisible());
    QCOMPARE(colorOf(line), tokenHex(window, "focusRing"));
}

void UiMainWindowTest::narrowQueueDropsAlbumBeforeTitle() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    // As narrow as the window goes; with the default 10 pt font a 1000 px
    // window is narrow enough.
    window->resize(window->minimumWidth(), 720);
    QTRY_COMPARE(window->width(), window->minimumWidth());
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *album = named(window, "queueHeader_album");
    QQuickItem *albumHandle = named(window, "queueColumnHandle_album");
    QQuickItem *title = named(window, "queueHeader_title");
    QVERIFY(queue && album && albumHandle && title);
    QTRY_VERIFY(!album->isVisible());
    QVERIFY(!albumHandle->isVisible());
    QVERIFY(!albumHandle->activeFocusOnTab());
    QVERIFY(title->width() >= queue->property("minimumTitleColumnWidth").toReal() - 1);
    // Artist keeps its quarter of the table.
    QQuickItem *artist = named(window, "queueHeader_artist");
    QVERIFY(std::abs(artist->width() + queue->property("columnGap").toReal() - queue->width() / 4) <= 1);
    // Wider, Album comes back.
    window->resize(1600, 1000);
    QTRY_VERIFY(album->isVisible());
    QVERIFY(albumHandle->isVisible());
}

void UiMainWindowTest::firstRunShowsOneMessage() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QCOMPARE(bridge->property("libraryRootCount").toInt(), 0);
    QCOMPARE(bridge->property("queueLength").toInt(), 0);

    // One message, the queue's: the app icon, the title, two lines and the
    // button that adds a library folder.
    QQuickItem *message = named(window, "queueFirstRunMessage");
    QVERIFY(shown(message));
    QQuickItem *icon = named(window, "queueFirstRunIcon");
    QCOMPARE(icon->width(), 48.0);
    // The app icon's own 48 px drawing, built in, so it shows whether or not
    // the icon is installed.
    QCOMPARE(icon->property("source").toUrl(), QUrl(QStringLiteral("qrc:/icons/hicolor/48x48/apps/ferrous.svg")));
    QTRY_VERIFY(icon->property("valid").toBool());
    QQuickItem *title = named(window, "queueFirstRunTitle");
    QCOMPARE(title->property("text").toString(), QStringLiteral("Start with your music folder"));
    QCOMPARE(title->property("font").value<QFont>().weight(), QFont::DemiBold);
    QQuickItem *explanation = named(window, "queueFirstRunExplanation");
    QCOMPARE(explanation->property("text").toString().count(QLatin1Char('\n')), 1);
    QCOMPARE(colorOf(explanation), tokenHex(window, "textSecondary"));
    QQuickItem *button = named(window, "firstRunAddFolderButton");
    QVERIFY(shown(button));
    QCOMPARE(qvariant_cast<QObject *>(button->property("action")),
             window->findChild<QObject *>(QStringLiteral("addLibraryFolderAction")));
    // Centred in the queue.
    QQuickItem *queue = named(window, "playlistView");
    QVERIFY(std::abs(sceneRect(message).center().x() - sceneRect(queue).center().x()) <= 1);
    // No other message: the library shows its header with nothing under it,
    // and the queue no empty line; the pane is its floor colour.
    QVERIFY(!named(window, "libraryEmptyStateLabel")->isVisible());
    QVERIFY(!named(window, "queueEmptyLabel")->isVisible());
    QVERIFY(shown(named(window, "libraryHeader")));
    QCOMPARE(itemNamed(named(window, "libraryHeader"), QStringLiteral("regionHeaderTotals"))
                 ->property("text").toString(),
             QString());
    QCOMPARE(itemNamed(named(window, "queueHeader"), QStringLiteral("regionHeaderTotals"))
                 ->property("text").toString(),
             QString());
    QCOMPARE(colorOf(named(window, "spectrogramPane")), QStringLiteral("#ff000000"));
    QVERIFY(shown(named(window, "nowPlayingEmptyRow")));
    // The empty queue's header shows only its name, and Tab skips its
    // resize handles.
    for (const char *column : {"queueHeader_artist", "queueHeader_album", "queueHeader_length"}) {
        QCOMPARE(named(window, column)->opacity(), 0.0);
    }
    QVERIFY(!named(window, "queueColumnHandle_artist")->activeFocusOnTab());

    // With a library folder, the first run is over: a quiet line in each.
    bridge->setProperty("libraryRootCount", 1);
    QTRY_VERIFY(!message->isVisible());
    QQuickItem *queueEmpty = named(window, "queueEmptyLabel");
    QTRY_VERIFY(shown(queueEmpty));
    QCOMPARE(queueEmpty->property("text").toString(), QStringLiteral("The queue is empty"));
    QQuickItem *libraryEmpty = named(window, "libraryEmptyStateLabel");
    QTRY_VERIFY(shown(libraryEmpty));
    QCOMPARE(libraryEmpty->property("text").toString(), QStringLiteral("Loading library…"));
    // Once the backend has sent its (empty) tree.
    QObject *libraryController = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(QMetaObject::invokeMethod(libraryController, "requestTreeApply", Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, QByteArray())));
    QTRY_COMPARE(libraryEmpty->property("text").toString(),
                 QStringLiteral("No music found in the library folders"));
    // Queued tracks replace the line, and the columns get their labels.
    bridge->setProperty("queueRows", sampleQueueRows(1));
    bridge->setProperty("queueLength", 1);
    QTRY_VERIFY(!queueEmpty->isVisible());
    QCOMPARE(named(window, "queueHeader_artist")->opacity(), 1.0);
    QVERIFY(named(window, "queueColumnHandle_artist")->activeFocusOnTab());
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
    UiMainWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

// The viewers in the main window open and close with the controls style's
// own popup transition, as every popup does (instant with animations off),
// and show at once rather than after a delay.
void UiMainWindowTest::viewersInTheWindowFadeAsTheStylesPopupsDo() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());

    // The style's popup, for its transitions.
    QQmlComponent stylePopup(&engine);
    stylePopup.setData("import QtQuick.Controls\nPopup {}\n", QUrl());
    QScopedPointer<QObject> reference(stylePopup.create());
    QVERIFY2(reference != nullptr, qPrintable(stylePopup.errorString()));
    const auto fadeDuration = [](QObject *popup, const char *transition) {
        auto *object = qvariant_cast<QObject *>(popup->property(transition));
        if (object == nullptr) {
            return -1;
        }
        QQmlListReference animations(object, "animations");
        return animations.count() > 0 ? animations.at(0)->property("duration").toInt() : -1;
    };
    // A popup makes its transitions as it first opens and closes.
    reference->setProperty("parent", QVariant::fromValue(window->contentItem()));
    QVERIFY(QMetaObject::invokeMethod(reference.data(), "open"));
    QTRY_VERIFY(reference->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(reference.data(), "close"));
    QTRY_VERIFY(!reference->property("visible").toBool());
    const int enterDuration = fadeDuration(reference.data(), "enter");
    const int exitDuration = fadeDuration(reference.data(), "exit");
    QVERIFY(enterDuration >= 0 && exitDuration >= 0);

    const QList<std::pair<const char *, const char *>> viewers{
        {"openSpectrogramViewer", "spectrogramViewerPopup"},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerPopup"},
    };
    for (const auto &[open, popupName] : viewers) {
        QObject *popup = window->findChild<QObject *>(QLatin1String(popupName));
        QVERIFY2(popup != nullptr, popupName);

        // Shown in the same turn of the event loop as the request.
        QVERIFY(QMetaObject::invokeMethod(viewer, open));
        QCoreApplication::processEvents();
        QVERIFY2(popup->property("visible").toBool(), popupName);
        QTRY_VERIFY2(popup->property("opened").toBool(), popupName);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY2(!popup->property("visible").toBool(), popupName);
        QCOMPARE(fadeDuration(popup, "enter"), enterDuration);
        QCOMPARE(fadeDuration(popup, "exit"), exitDuration);
    }
}

#include "tst_ui_main_window.moc"
