// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// The library tree, the queue and global search.
class LibraryQueueSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void libraryTreeStartsCollapsedByDefault();
    void libraryTreeRowsCarryTheirCoverStamp();
    void rootRowsStartExpandedByDefault();
    void artistExpansionPopulatesInBatches();
    void lazyArtistRowRequestsBackendExpansion();
    void libraryTreeSeedsExpandedKeysBeforeFirstFrame();
    void libraryTreeCollapsesAllExpandedBranches();
    void artistPrefixSearchUsesModelLookup();
    void libraryTreeAppliesALargeUpdateWithoutAReset();
    void libraryTreeMovesRowsWithoutAReset();
    void libraryTreeFollowsRootChanges();
    void libraryControllerExtendsSelectionWithShiftArrows();
    void libraryControllerRestoresExpandedSelectionAndViewport();
    void libraryControllerUsesDelegateCoordinatesForViewportAnchor();
    void libraryControllerForwardsShowTrackRequest();
    void libraryControllerCentersBridgeRevealAfterExpansion();
    void shiftSelectingTenThousandLibraryRowsStaysFast();
    void librarySelectionFollowsItsRowsThroughTreeUpdates();
    void playingALibrarySelectionSendsOneBatch();
    void revealKeyExpandsADeepKeyInOneCall();
    void revealKeyWaitsForRowsTheBackendHasNotSentWithoutTimers();
    void queueMovesAndDropsActOnEveryHighlightedRow();
    void queueSelectionFollowsRowRangeMoves();
    void emptyLibraryAndFileMenuAddALibraryFolder();
    void draggingAHighlightedQueueRowMovesEveryHighlightedRow();
    void queueRowsShowTheirStatesInThePaletteColours();
    void queueDropsFollowTheRowsAfterRowsAboveAreRemoved();
    void aRevealDuringTheSavedViewRestoreKeepsSavingTheView();
    void escapeCancelsAQueueRowDrag();
    void aRevealBeforeTheFirstTreeWinsOverTheSavedViewRestore();
    void trackIdentityChangedSignalTriggersQmlHandler();
    void queueAutoCenterIsDeferredOffHandlerStack();
    void queueRemovalSendsOneBatch();
    void queueEditsKeepSelectionAndViewport();
    void queueRemovalsKeepActionsOnHighlightedRows();
    void queueContainIndexSkipsScrollWhenVisible();
    void queueContainIndexScrollsUpWhenAboveViewport();
    void queueContainIndexClampsAtListEnd();
    void wheelScrollsListsByTheWheelDelta();
    void queueKeysReachBothEndsAndOpenTheContextMenu();
    void deleteRemovesQueueTracksOnlyWhileTheQueueHasFocus();
    void queueChangesShowNoNoticeAndEditUndoRestoresThem();
    void removingALibraryRootAsksFirstAndNamesIt();
    void libraryKeysReachBothEndsAndOpenTheContextMenu();
    void libraryRowsLayOutTheirYearsWithoutWarnings();
    void editMenuNamesTheUndoOfAMoveAndItSelectsTheMovedRows();
};

namespace {

QByteArray artistWithManyAlbumsBinary(int albumCount) {
    QVector<BinaryTreeRow> rows;
    rows.reserve(1 + albumCount * 2);
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        static_cast<quint32>(albumCount),
        QStringLiteral("Artist A (%1)").arg(albumCount),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });

    for (int i = 0; i < albumCount; ++i) {
        const QString albumName = QStringLiteral("Album %1").arg(i + 1);
        const QString albumPath = QStringLiteral("/music/artist/%1").arg(albumName.toLower().replace(' ', ""));
        const QString trackPath = albumPath + QStringLiteral("/track.flac");
        rows.push_back(BinaryTreeRow{
            2,
            1,
            i,
            0,
            1,
            albumName,
            QStringLiteral("album|Artist A|%1").arg(albumName),
            QStringLiteral("Artist A"),
            albumPath,
            albumPath + QStringLiteral("/cover.jpg"),
            {},
            {},
        });
        rows.push_back(BinaryTreeRow{
            4,
            2,
            -1,
            1,
            0,
            QStringLiteral("01 - Track %1").arg(i + 1),
            QStringLiteral("track|%1").arg(trackPath),
            QStringLiteral("Artist A"),
            trackPath,
            {},
            trackPath,
            QStringList{trackPath},
        });
    }

    return encodeRows(rows);
}

QByteArray artistOnlyLazyBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        2,
        QStringLiteral("Artist A (2)"),
        QStringLiteral("artist|/music|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

QByteArray multiRootBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-a"),
        QStringLiteral("root|/music-a"),
        {},
        QStringLiteral("/music-a"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist A (0)"),
        QStringLiteral("artist|/music-a|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music-a/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-b"),
        QStringLiteral("root|/music-b"),
        {},
        QStringLiteral("/music-b"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist B (0)"),
        QStringLiteral("artist|/music-b|Artist B"),
        QStringLiteral("Artist B"),
        QStringLiteral("/music-b/Artist B"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

// A tree like the bridge sends for expanded artists: under `root`, each
// artist in `artists` has one album of `tracksPerAlbum` tracks. `title`
// names each track. With `withRootRow`, the root has a row of its own.
QVector<BinaryTreeRow> artistAlbumRows(
    const QString &root,
    const QVector<int> &artists,
    int tracksPerAlbum,
    const std::function<QString(int, int)> &title,
    bool withRootRow = false) {
    QVector<BinaryTreeRow> rows;
    const quint16 base = withRootRow ? 1 : 0;
    if (withRootRow) {
        rows.push_back(BinaryTreeRow{0, 0, -1, 0, static_cast<quint32>(artists.size()), root,
            QStringLiteral("root|%1").arg(root), {}, root, {}, {}, {}});
    }
    for (int artist : artists) {
        const QString name = QStringLiteral("Artist %1").arg(artist, 4, 10, QLatin1Char('0'));
        const QString artistPath = root + QLatin1Char('/') + name;
        const QString albumPath = artistPath + QStringLiteral("/Album");
        rows.push_back(BinaryTreeRow{1, base, -1, 0, 1, name + QStringLiteral(" (1)"),
            QStringLiteral("artist|%1|%2").arg(root, name), name, artistPath, {}, {}, {}});
        rows.push_back(BinaryTreeRow{2, static_cast<quint16>(base + 1), -1, 0,
            static_cast<quint32>(tracksPerAlbum), QStringLiteral("Album"),
            QStringLiteral("album|%1|%2|Album").arg(root, name), name, albumPath,
            albumPath + QStringLiteral("/cover.jpg"), {}, {}});
        for (int track = 1; track <= tracksPerAlbum; ++track) {
            const QString path = albumPath + QStringLiteral("/%1.flac").arg(track, 2, 10, QLatin1Char('0'));
            rows.push_back(BinaryTreeRow{4, static_cast<quint16>(base + 2), -1,
                static_cast<quint32>(track), 0, title(artist, track),
                QStringLiteral("track|%1").arg(path), name, path, {}, path, QStringList{path}});
        }
    }
    return rows;
}

QVector<int> artistRange(int first, int last) {
    QVector<int> artists;
    for (int artist = first; artist <= last; ++artist) {
        artists.push_back(artist);
    }
    return artists;
}

QStringList expandedArtistAlbumKeys(const QString &root, const QVector<int> &artists) {
    QStringList keys;
    for (int artist : artists) {
        const QString name = QStringLiteral("Artist %1").arg(artist, 4, 10, QLatin1Char('0'));
        keys.push_back(QStringLiteral("artist|%1|%2").arg(root, name));
        keys.push_back(QStringLiteral("album|%1|%2|Album").arg(root, name));
    }
    return keys;
}

QString plainTrackTitle(int, int track) {
    return QStringLiteral("%1 - Track").arg(track, 2, 10, QLatin1Char('0'));
}

// Applies `treeBytes` and waits for the model to take it.
bool applyLibraryTree(LibraryTreeModel &model, const QByteArray &treeBytes) {
    QSignalSpy applied(&model, &LibraryTreeModel::treeApplied);
    model.setLibraryTreeFromBinary(treeBytes);
    return applied.count() > 0 || applied.wait(10000);
}

// Every row of `model`, as a list of its roles, to compare with a model
// that parsed the same tree from scratch.
QVector<QVariantMap> modelRows(const LibraryTreeModel &model) {
    QVector<QVariantMap> rows;
    rows.reserve(model.rowCount());
    for (int row = 0; row < model.rowCount(); ++row) {
        rows.push_back(model.rowDataForRow(row));
    }
    return rows;
}

// Drop the captured messages that are `message`, which the test expects.
void dropCapturedMessages(const QString &message) {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().removeAll(message);
}

} // namespace

void LibraryQueueSmokeTest::libraryTreeRowsCarryTheirCoverStamp() {
    LibraryTreeModel model;
    model.setExpandedKeys(QStringList{QStringLiteral("artist|Artist A")});
    // Above 2^53, which a QML number could not hold exactly.
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary(0x0102030405060708ULL));
    QTRY_COMPARE(model.rowCount(), 2);
    const QModelIndex album = model.index(1, 0);
    QCOMPARE(model.data(album, LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("album"));
    QCOMPARE(model.data(album, LibraryTreeModel::CoverStampRole).toString(),
             QString::number(0x0102030405060708ULL));
    QVERIFY(model.roleNames().value(LibraryTreeModel::CoverStampRole) == "coverStamp");

    // The cover was replaced at the same path: the row changes in place, so
    // its delegate binds the new stamp and loads the new image.
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary(9));
    QTRY_COMPARE(model.data(album, LibraryTreeModel::CoverStampRole).toString(), QStringLiteral("9"));
    QCOMPARE(reset.count(), 0);
    QVERIFY(std::any_of(changed.cbegin(), changed.cend(), [](const QList<QVariant> &arguments) {
        const auto topLeft = arguments.at(0).value<QModelIndex>();
        const auto bottomRight = arguments.at(1).value<QModelIndex>();
        return topLeft.row() <= 1 && bottomRight.row() >= 1;
    }));
    QCOMPARE(model.data(album, LibraryTreeModel::CoverPathRole).toString(),
             QStringLiteral("/music/Artist A/Album A/cover.jpg"));
}

void LibraryQueueSmokeTest::libraryTreeStartsCollapsedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());

    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void LibraryQueueSmokeTest::rootRowsStartExpandedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(1, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
    QCOMPARE(model.data(model.index(2, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(3, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void LibraryQueueSmokeTest::artistExpansionPopulatesInBatches() {
    LibraryTreeModel model;

    model.setLibraryTreeFromBinary(artistWithManyAlbumsBinary(80));
    QTRY_COMPARE(model.rowCount(), 1);

    model.toggleArtist(QStringLiteral("Artist A"));

    QTRY_COMPARE(model.rowCount(), 81);
}

void LibraryQueueSmokeTest::lazyArtistRowRequestsBackendExpansion() {
    LibraryTreeModel model;
    QSignalSpy spy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));

    model.setLibraryTreeFromBinary(artistOnlyLazyBinary());
    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), false);

    model.toggleArtist(QStringLiteral("Artist A"));
    QTRY_COMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.value(0).toString(), QStringLiteral("artist|/music|Artist A"));
    QCOMPARE(args.value(1).toBool(), true);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
}

void LibraryQueueSmokeTest::libraryTreeSeedsExpandedKeysBeforeFirstFrame() {
    LibraryTreeModel model;
    QSignalSpy expansionSpy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));

    model.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());

    QTRY_COMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
    QCOMPARE(model.data(model.index(1, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
    QCOMPARE(expansionSpy.count(), 0);
}

void LibraryQueueSmokeTest::libraryTreeCollapsesAllExpandedBranches() {
    LibraryTreeModel model;
    model.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());
    QTRY_COMPARE(model.rowCount(), 3);

    QSignalSpy expansionSpy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));
    model.collapseAll();

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(expansionSpy.count(), 2);
    for (const QList<QVariant> &arguments : expansionSpy) {
        QCOMPARE(arguments.value(1).toBool(), false);
    }

    LibraryTreeModel rootedModel;
    rootedModel.setLibraryTreeFromBinary(multiRootBinary());
    QTRY_COMPARE(rootedModel.rowCount(), 4);
    rootedModel.collapseAll();
    QCOMPARE(rootedModel.rowCount(), 2);
}

void LibraryQueueSmokeTest::artistPrefixSearchUsesModelLookup() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist b"), 0), 3);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist a"), 2), 1);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("missing"), 0), -1);
}

void LibraryQueueSmokeTest::libraryTreeAppliesALargeUpdateWithoutAReset() {
    const QString root = QStringLiteral("/music");
    // 500 expanded artists of 38 tracks: 20,000 rows.
    const QVector<int> before = artistRange(200, 699);
    // 125 new artists sort before them, adding 5,000 rows above the view;
    // the last 10 artists go, and one artist's titles change.
    QVector<int> after = artistRange(0, 124);
    after += artistRange(200, 689);
    const auto changedTitles = [](int artist, int track) {
        return artist == 400 ? QStringLiteral("%1 - Retagged").arg(track) : plainTrackTitle(artist, track);
    };
    QStringList expanded = expandedArtistAlbumKeys(root, before);
    expanded += expandedArtistAlbumKeys(root, artistRange(0, 124));

    LibraryTreeModel model;
    model.setExpandedKeys(expanded);
    QVERIFY(applyLibraryTree(model, encodeRows(artistAlbumRows(root, before, 38, plainTrackTitle))));
    QCOMPARE(model.rowCount(), 20000);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> window(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Window 2.15

Window {
    width: 320
    height: 240
    visible: true
    property alias view: view

    // As the library pane's view: reused delegates bound to the roles.
    ListView {
        id: view
        anchors.fill: parent
        model: testLibraryModel
        reuseItems: true
        cacheBuffer: 200
        delegate: Rectangle {
            property string rowKey: selectionKey || ""
            property bool rowExpanded: expanded
            width: ListView.view.width
            height: 24
            color: index % 2 === 0 ? "white" : "lightgray"
            Text { text: title || "" }
        }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(window != nullptr, qPrintable(errorText));
    auto *view = qvariant_cast<QQuickItem *>(window->property("view"));
    QVERIFY(view != nullptr);

    const QString currentKey = QStringLiteral("track|/music/Artist 0450/Album/05.flac");
    const QString expandedArtist = QStringLiteral("artist|/music|Artist 0450");
    const int currentRow = model.indexForSelectionKey(currentKey);
    QVERIFY(currentRow > 0);
    view->setProperty("currentIndex", currentRow);
    QVERIFY(QMetaObject::invokeMethod(
        view, "positionViewAtIndex", Q_ARG(int, currentRow), Q_ARG(int, 0 /* ListView.Beginning */)));
    const auto topKey = [&]() {
        int top = -1;
        QMetaObject::invokeMethod(view, "indexAt", Q_RETURN_ARG(int, top),
            Q_ARG(qreal, 1.0), Q_ARG(qreal, view->property("contentY").toReal() + 1.0));
        return model.selectionKeyForRow(top);
    };
    QTRY_COMPARE(topKey(), currentKey);

    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removals(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
    const QByteArray next = encodeRows(artistAlbumRows(root, after, 38, changedTitles));
    QVERIFY(applyLibraryTree(model, next));

    QCOMPARE(resets.count(), 0);
    QVERIFY(inserts.count() > 0);
    QVERIFY(removals.count() > 0);
    QVERIFY(changes.count() > 0);
    QCOMPARE(model.rowCount(), 20000 + 5000 - 400);
    // The view keeps its current item and what it shows, once it lays out
    // the changes.
    QTRY_COMPARE(view->property("currentIndex").toInt(), currentRow + 5000);
    QCOMPARE(model.selectionKeyForRow(view->property("currentIndex").toInt()), currentKey);
    QTRY_COMPARE(topKey(), currentKey);
    const int artistRow = model.indexForSelectionKey(expandedArtist);
    QVERIFY(model.data(model.index(artistRow, 0), LibraryTreeModel::ExpandedRole).toBool());
    const int retagged = model.indexForSelectionKey(QStringLiteral("track|/music/Artist 0400/Album/07.flac"));
    QCOMPARE(model.data(model.index(retagged, 0), LibraryTreeModel::TitleRole).toString(),
        QStringLiteral("7 - Retagged"));

    // The rows are those of a model that parsed the tree from scratch.
    LibraryTreeModel fresh;
    fresh.setExpandedKeys(expanded);
    QVERIFY(applyLibraryTree(fresh, next));
    QVERIFY(modelRows(model) == modelRows(fresh));
}

void LibraryQueueSmokeTest::libraryTreeMovesRowsWithoutAReset() {
    const QString root = QStringLiteral("/music");
    const QVector<int> artists = artistRange(0, 199);
    LibraryTreeModel model;
    model.setExpandedKeys(expandedArtistAlbumKeys(root, artists));
    QVERIFY(applyLibraryTree(model, encodeRows(artistAlbumRows(root, artists, 10, plainTrackTitle))));
    const QPersistentModelIndex kept(model.index(model.indexForSelectionKey(
        QStringLiteral("track|/music/Artist 0150/Album/03.flac")), 0));
    QVERIFY(kept.isValid());

    // As a sort order or rename that changes where rows go: the first
    // artists' rows come last, and one album's tracks reverse.
    QVector<BinaryTreeRow> rows = artistAlbumRows(root, artistRange(5, 199), 10, plainTrackTitle);
    rows += artistAlbumRows(root, artistRange(0, 4), 10, plainTrackTitle);
    const int album = static_cast<int>(std::find_if(rows.begin(), rows.end(), [](const BinaryTreeRow &row) {
        return row.key == QStringLiteral("album|/music|Artist 0020|Album");
    }) - rows.begin());
    std::reverse(rows.begin() + album + 1, rows.begin() + album + 11);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    const QByteArray next = encodeRows(rows);
    QVERIFY(applyLibraryTree(model, next));

    QCOMPARE(resets.count(), 0);
    QVERIFY(kept.isValid());
    QCOMPARE(kept.data(LibraryTreeModel::SelectionKeyRole).toString(),
        QStringLiteral("track|/music/Artist 0150/Album/03.flac"));
    LibraryTreeModel fresh;
    fresh.setExpandedKeys(expandedArtistAlbumKeys(root, artists));
    QVERIFY(applyLibraryTree(fresh, next));
    QVERIFY(modelRows(model) == modelRows(fresh));
}

void LibraryQueueSmokeTest::libraryTreeFollowsRootChanges() {
    const QVector<int> artists = artistRange(0, 9);
    QStringList expanded = expandedArtistAlbumKeys(QStringLiteral("/music"), artists);
    expanded += expandedArtistAlbumKeys(QStringLiteral("/more"), artists);
    const QByteArray single = encodeRows(artistAlbumRows(QStringLiteral("/music"), artists, 3, plainTrackTitle));
    QVector<BinaryTreeRow> twoRoots = artistAlbumRows(QStringLiteral("/more"), artists, 3, plainTrackTitle, true);
    twoRoots += artistAlbumRows(QStringLiteral("/music"), artists, 3, plainTrackTitle, true);
    const QByteArray multi = encodeRows(twoRoots);
    const QByteArray other = encodeRows(artistAlbumRows(QStringLiteral("/more"), artists, 3, plainTrackTitle));

    LibraryTreeModel model;
    model.setExpandedKeys(expanded);
    QVERIFY(applyLibraryTree(model, single));
    for (const QByteArray &tree : {multi, single, other, QByteArray{}, single}) {
        QVERIFY(applyLibraryTree(model, tree));
        LibraryTreeModel fresh;
        fresh.setExpandedKeys(expanded);
        QVERIFY(applyLibraryTree(fresh, tree));
        QVERIFY(modelRows(model) == modelRows(fresh));
    }
    // A root added beside the first keeps its artists' rows, at a new depth.
    QVERIFY(applyLibraryTree(model, single));
    const QPersistentModelIndex artist(model.index(0, 0));
    QVERIFY(applyLibraryTree(model, multi));
    QVERIFY(artist.isValid());
    QCOMPARE(artist.data(LibraryTreeModel::SelectionKeyRole).toString(),
        QStringLiteral("artist|/music|Artist 0000"));
    QCOMPARE(artist.data(LibraryTreeModel::DepthRole).toInt(), 1);
}

void LibraryQueueSmokeTest::libraryControllerExtendsSelectionWithShiftArrows() {
    LibraryTreeModel model;
    model.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());
    QTRY_COMPARE(model.rowCount(), 3);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    property alias controllerRef: controller
    property bool lastAccepted: false

    function pressShiftDown() {
        const event = { key: Qt.Key_Down, modifiers: Qt.ShiftModifier, text: "", accepted: false }
        controller.handleKeyPress(event)
        lastAccepted = event.accepted
    }

    function pressShiftUp() {
        const event = { key: Qt.Key_Up, modifiers: Qt.ShiftModifier, text: "", accepted: false }
        controller.handleKeyPress(event)
        lastAccepted = event.accepted
    }

    QtObject {
        id: bridge
        property int libraryTrackCount: 1
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
    }
    QtObject { id: tagEditorApi }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Component.onCompleted: controller.selectIndex(0)
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QVERIFY(controller != nullptr);
    QCOMPARE(model.selectedKeys(), QStringList({
        QStringLiteral("artist|Artist A"),
    }));

    QVERIFY(QMetaObject::invokeMethod(root.get(), "pressShiftDown"));
    QCOMPARE(root->property("lastAccepted").toBool(), true);
    QCOMPARE(model.selectedKeys(), QStringList({
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    }));

    QVERIFY(QMetaObject::invokeMethod(root.get(), "pressShiftDown"));
    QCOMPARE(model.selectedKeys(), QStringList({
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
        QStringLiteral("track|/music/artist/album/track01.flac"),
    }));

    QVERIFY(QMetaObject::invokeMethod(root.get(), "pressShiftUp"));
    QCOMPARE(model.selectedKeys(), QStringList({
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    }));
    QCOMPARE(model.anchorRow(), 0);
}

void LibraryQueueSmokeTest::libraryControllerRestoresExpandedSelectionAndViewport() {
    LibraryTreeModel model;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    engine.rootContext()->setContextProperty(
        QStringLiteral("treeBytes"),
        sampleArtistAlbumTreeBinary());
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    width: 320
    height: 24
    property alias controllerRef: controller
    property alias viewRef: view
    property alias saveCount: bridge.saveCount
    property alias restoreStartedBeforePublish: publishTimer.restoreStartedBeforePublish

    QtObject {
        id: bridge
        property int libraryTrackCount: 1
        property bool libraryViewStateAvailable: false
        property var libraryExpandedKeys: ["artist|Artist A", "album|Artist A|Album A"]
        property string libraryViewSelectionKey: "track|/music/artist/album/track01.flac"
        property string libraryViewAnchorKey: "album|Artist A|Album A"
        property real libraryViewAnchorOffset: 3
        property int saveCount: 0
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) { saveCount += 1 }
        function setLibraryNodeExpanded(key, expanded) {}
        function showTrackInLibrary(path) {}
    }

    QtObject {
        id: tagEditorApi
        function openSelection(selections) { return false }
    }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    ListView {
        id: view
        readonly property real rowHeight: 24
        anchors.fill: parent
        model: testLibraryModel
        delegate: Item { width: view.width; height: view.rowHeight }
        Component.onCompleted: controller.registerView(view)
    }

    Timer {
        id: publishTimer
        interval: 40
        repeat: false
        property bool restoreStartedBeforePublish: false
        onTriggered: {
            restoreStartedBeforePublish = controller.restoredViewStateApplied
            bridge.libraryViewStateAvailable = true
            controller.tryApplyPersistedViewState()
        }
    }

    Component.onCompleted: {
        controller.requestTreeApply(1, treeBytes)
        controller.tryApplyPersistedViewState()
        publishTimer.start()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QObject *view = qvariant_cast<QObject *>(root->property("viewRef"));
    QVERIFY(controller != nullptr);
    QVERIFY(view != nullptr);
    QTRY_COMPARE(model.rowCount(), 3);
    QCOMPARE(root->property("restoreStartedBeforePublish").toBool(), false);
    QTRY_COMPARE(
        controller->property("selectedSelectionKey").toString(),
        QStringLiteral("track|/music/artist/album/track01.flac"));
    QTRY_VERIFY(std::abs(view->property("contentY").toDouble() - 27.0) < 0.5);
    QTRY_COMPARE(root->property("saveCount").toInt(), 1);
}

void LibraryQueueSmokeTest::libraryControllerUsesDelegateCoordinatesForViewportAnchor() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    property alias controllerRef: controller
    property alias contentY: mockView.contentY
    property var capturedAnchor: ({})

    function restoreCapturedAnchor() {
        mockView.contentY = 0
        controller.restoreViewAnchor(capturedAnchor)
    }

    QtObject {
        id: bridge
        property int libraryTrackCount: 1
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
    }
    QtObject {
        id: model
        property int count: 20
        function selectionKeyForRow(row) { return row === 10 ? "anchor-key" : "other-key" }
        function indexForSelectionKey(key) { return key === "anchor-key" ? 10 : -1 }
    }
    QtObject { id: tagEditorApi }
    QtObject {
        id: mockView
        property real contentY: 350
        property real contentHeight: 1000
        property real height: 100
        property real originY: 100
        property real rowHeight: 24
        property bool activeFocus: false
        function indexAt(x, y) { return 10 }
        function itemAtIndex(index) { return index === 10 ? ({ y: 340 }) : null }
        function positionViewAtIndex(index, mode) { contentY = index === 10 ? 340 : 0 }
    }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: model
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Component.onCompleted: {
        controller.registerView(mockView)
        capturedAnchor = controller.captureViewAnchor()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    const QVariantMap anchor = root->property("capturedAnchor").toMap();
    QCOMPARE(anchor.value(QStringLiteral("key")).toString(), QStringLiteral("anchor-key"));
    QCOMPARE(anchor.value(QStringLiteral("offset")).toDouble(), 10.0);
    QVERIFY(QMetaObject::invokeMethod(root.get(), "restoreCapturedAnchor"));
    QTRY_COMPARE(root->property("contentY").toDouble(), 350.0);
}

void LibraryQueueSmokeTest::libraryControllerForwardsShowTrackRequest() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    property alias controllerRef: controller
    property alias shownPath: bridge.shownPath

    QtObject {
        id: bridge
        property string shownPath: ""
        function showTrackInLibrary(path) { shownPath = path }
    }
    QtObject {
        id: model
        property int count: 0
    }
    QtObject {
        id: tagEditorApi
    }
    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: model
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QVERIFY(controller != nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "showTrackInLibrary",
        Q_ARG(QVariant, QStringLiteral("/music/song.flac"))));
    QCOMPARE(root->property("shownPath").toString(), QStringLiteral("/music/song.flac"));
}

void LibraryQueueSmokeTest::libraryControllerCentersBridgeRevealAfterExpansion() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(artistWithManyAlbumsBinary(20));
    QTRY_COMPARE(model.rowCount(), 1);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    width: 320
    height: 120
    property alias controllerRef: controller
    property alias viewRef: view

    QtObject {
        id: bridge
        property int libraryTrackCount: 20
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
        function setLibraryNodeExpanded(key, expanded) {}
        function showTrackInLibrary(path) {}
    }
    QtObject { id: tagEditorApi }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    ListView {
        id: view
        readonly property real rowHeight: 24
        anchors.fill: parent
        model: testLibraryModel
        delegate: Item { width: view.width; height: view.rowHeight }
        Component.onCompleted: controller.registerView(view)
    }

    function reveal() {
        controller.requestBridgeReveal(
            ["artist|Artist A", "album|Artist A|Album 20"],
            "track|/music/artist/album20/track.flac")
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QObject *view = qvariant_cast<QObject *>(root->property("viewRef"));
    QVERIFY(controller != nullptr);
    QVERIFY(view != nullptr);
    // The rows are in the model, so the reveal selects and centers the
    // track in the call, with no retries.
    QVERIFY(QMetaObject::invokeMethod(root.get(), "reveal"));
    QCOMPARE(
        controller->property("selectedSelectionKey").toString(),
        QStringLiteral("track|/music/artist/album20/track.flac"));
    QVERIFY(view->property("contentY").toDouble() > 300.0);
}

void LibraryQueueSmokeTest::trackIdentityChangedSignalTriggersQmlHandler() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15

Item {
    id: harness
    property int trackIdentityChangedCount: 0

    QtObject {
        id: bridge
        objectName: "bridge"
        property int playingQueueIndex: -1
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
    }

    Connections {
        target: bridge
        function onTrackIdentityChanged() {
            harness.trackIdentityChangedCount++
        }
    }

    function emitTrackIdentityChanged() {
        bridge.playingQueueIndex = 5
        bridge.trackIdentityChanged()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QCOMPARE(root->property("trackIdentityChangedCount").toInt(), 0);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "emitTrackIdentityChanged"));
    QCOMPARE(root->property("trackIdentityChangedCount").toInt(), 1);
}

void LibraryQueueSmokeTest::queueRemovalSendsOneBatch() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers
Item {
    id: harness
    property int batches: 0
    property string removed: ""
    QtObject {
        id: bridge
        property int queueLength: 10000
        property int selectedQueueIndex: 1
        function removeIndices(indices) { harness.batches++; harness.removed = indices.join(",") }
    }
    Controllers.QueueController {
        id: controller
        uiBridge: bridge
        tagEditorApi: null
        openTagEditorDialog: function() {}
    }
    function removeSelection() {
        controller.setSelectedIndices([1, 200, 9999])
        controller.removeSelectedTrack()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QVERIFY(QMetaObject::invokeMethod(root.data(), "removeSelection"));
    QCOMPARE(root->property("batches").toInt(), 1);
    QCOMPARE(root->property("removed").toString(), QStringLiteral("9999,200,1"));
}

void LibraryQueueSmokeTest::queueEditsKeepSelectionAndViewport() {
    QueueRowsModel rows;
    const auto row = [](int number) {
        QueueRowData data;
        data.title = QStringLiteral("Track %1").arg(number);
        data.path = QStringLiteral("/music/%1.flac").arg(number);
        data.lengthText = QStringLiteral("1:00");
        return data;
    };
    QVector<QueueRowData> initial;
    for (int number = 0; number < 200; ++number) {
        initial.push_back(row(number));
    }
    rows.setRows(initial);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("queueRowsModel"), &rows);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "controllers" as Controllers
import "panes" as Panes

Window {
    id: harness
    width: 900
    height: 400
    visible: true
    property alias controller: queueController

    QtObject {
        id: bridge
        property var queueRows: queueRowsModel
        property int queueLength: 1000
        property int selectedQueueIndex: 50
        property int playingQueueIndex: -1
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        property bool profileLogsEnabled: false
        signal snapshotChanged()
        signal trackIdentityChanged()
        function selectQueueIndex(index) { selectedQueueIndex = index }
        function queuePathAt(index) { return "" }
        function removeAt(index) {}
        function removeIndices(indices) {}
        function moveQueue(from, to) {}
        function clearQueue() {}
        function openContainingFolder(path) {}
    }

    Components.UiPalette {
        id: palette
        windowRoot: harness
    }

    Controllers.QueueController {
        id: queueController
        objectName: "queueController"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(selection) { return false } }
        openTagEditorDialog: function() {}
        autoCenterSelection: false
    }

    QtObject {
        id: libraryControllerStub
        function insertRowItems(rowMap, row) {}
        function showTrackInLibrary(path) {}
    }

    Panes.QueuePane {
        anchors.fill: parent
        controller: queueController
        uiBridge: bridge
        uiPalette: palette
        preferredHeight: height
        playlistIndicatorColumnWidth: 22
        playlistOrderColumnWidth: 34
        playlistOrderText: function(index) { return String(index + 1) }
        libraryController: libraryControllerStub
        stepScrollView: function(view, wheel, stepSize, stepsPerWheel) {}
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        droppedExternalPaths: function(drop) { return [] }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *controller = root->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(controller != nullptr);
    QTRY_VERIFY(controller->property("view").value<QObject *>() != nullptr);
    auto *view = qobject_cast<QQuickItem *>(controller->property("view").value<QObject *>());
    QVERIFY(view != nullptr);
    QTRY_VERIFY(view->height() > 0);

    const auto selection = [controller]() {
        QList<int> indices;
        for (const QVariant &value : controller->property("selectedIndices").toList()) {
            indices.push_back(value.toInt());
        }
        return indices;
    };
    // The title of the row at the top of the viewport.
    const auto topRowTitle = [view, &rows]() {
        int index = -1;
        QMetaObject::invokeMethod(view, "indexAt", Q_RETURN_ARG(int, index),
            Q_ARG(qreal, 10.0), Q_ARG(qreal, view->property("contentY").toReal() + 1.0));
        const QueueRowData *top = rows.rowAt(index);
        return top != nullptr ? top->title : QString{};
    };

    const QVariantList selected{40, 60, 120};
    QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
        Q_ARG(QVariant, QVariant(selected))));
    controller->setProperty("selectionAnchorIndex", 60);
    view->setProperty("contentY", 50 * view->property("rowHeight").toReal());
    QTRY_COMPARE(topRowTitle(), QStringLiteral("Track 50"));
    QSignalSpy resets(&rows, &QAbstractItemModel::modelReset);

    // Edits elsewhere in the queue keep both where the user left them.
    QVERIFY(rows.removeRowsAt(10, 1));
    QCOMPARE(selection(), (QList<int>{39, 59, 119}));
    QVERIFY(rows.insertRowsAt(5, {row(1000), row(1001)}));
    QCOMPARE(selection(), (QList<int>{41, 61, 121}));
    QVERIFY(rows.moveRow(150, 0));
    QCOMPARE(selection(), (QList<int>{42, 62, 122}));
    QVERIFY(rows.moveRow(62, 190));
    QCOMPARE(selection(), (QList<int>{42, 122 - 1, 190}));
    QVERIFY(rows.updateRow(42, row(2000)));
    QCOMPARE(selection(), (QList<int>{42, 121, 190}));
    QCOMPARE(controller->property("selectionAnchorIndex").toInt(), 190);
    QCoreApplication::processEvents();
    QCOMPARE(topRowTitle(), QStringLiteral("Track 50"));

    // Removing a selected row drops only that row from the selection.
    QVERIFY(rows.removeRowsAt(121, 1));
    QCOMPARE(selection(), (QList<int>{42, 189}));
    // Removing a row in view, as the context menu does, keeps the rows
    // above it in place.
    int topIndex = -1;
    QMetaObject::invokeMethod(view, "indexAt", Q_RETURN_ARG(int, topIndex),
        Q_ARG(qreal, 10.0), Q_ARG(qreal, view->property("contentY").toReal() + 1.0));
    QVERIFY(rows.removeRowsAt(topIndex + 2, 1));
    QCoreApplication::processEvents();
    QCOMPARE(topRowTitle(), QStringLiteral("Track 50"));
    QCOMPARE(resets.count(), 0);

    // A whole new queue starts from the backend's selection.
    rows.setRows(initial);
    QCOMPARE(resets.count(), 1);
    QCOMPARE(selection(), (QList<int>{50}));
}

void LibraryQueueSmokeTest::queueRemovalsKeepActionsOnHighlightedRows() {
    QueueRowsModel rows;
    const auto row = [](int number) {
        QueueRowData data;
        data.title = QStringLiteral("Track %1").arg(number);
        data.path = QStringLiteral("/music/%1.flac").arg(number);
        return data;
    };
    const auto load = [&rows, &row](int count) {
        QVector<QueueRowData> initial;
        for (int number = 0; number < count; ++number) {
            initial.push_back(row(number));
        }
        rows.setRows(initial);
    };

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("queueRowsModel"), &rows);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers
Item {
    id: harness
    property var commands: []
    property alias bridge: bridge
    property alias controller: controller
    QtObject {
        id: bridge
        objectName: "bridge"
        property var queueRows: queueRowsModel
        property int queueLength: 0
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        function record(command) { harness.commands = harness.commands.concat([command]) }
        function selectQueueIndex(index) {
            if (index < 0 || index === selectedQueueIndex) return
            selectedQueueIndex = index
            record("select " + index)
        }
        function playAt(index) { record("play " + index) }
        function removeAt(index) { record("remove " + index) }
        function removeIndices(indices) { record("remove " + indices.join(",")) }
        function moveQueue(from, to) { record("move " + from + "," + to) }
        function shiftQueueRows(rows, direction) {
            record("shift " + rows.join(",") + " by " + direction)
        }
        function clearQueue() { record("clear") }
        function queuePathAt(index) { return "" }
    }
    Controllers.QueueController {
        id: controller
        uiBridge: bridge
        tagEditorApi: null
        openTagEditorDialog: function() {}
        autoCenterSelection: false
    }
    // Delete as the focused queue view gets it.
    function pressDelete() {
        controller.handlePlaylistKeyPress({ key: Qt.Key_Delete, modifiers: Qt.NoModifier })
    }
    function pressEnter() {
        controller.handlePlaylistKeyPress({ key: Qt.Key_Return, modifiers: Qt.NoModifier })
    }
    function takeCommands() {
        const taken = harness.commands.join("; ")
        harness.commands = []
        return taken
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *bridge = root->property("bridge").value<QObject *>();
    QObject *controller = root->property("controller").value<QObject *>();
    QVERIFY(bridge != nullptr);
    QVERIFY(controller != nullptr);

    const auto selection = [controller]() {
        QList<int> indices;
        for (const QVariant &value : controller->property("selectedIndices").toList()) {
            indices.push_back(value.toInt());
        }
        return indices;
    };
    const auto call = [&root](const char *method) {
        QVERIFY(QMetaObject::invokeMethod(root.data(), method));
    };
    const auto takeCommands = [&root]() {
        QVariant taken;
        QMetaObject::invokeMethod(root.data(), "takeCommands", Q_RETURN_ARG(QVariant, taken));
        return taken.toString();
    };
    const auto titleAt = [&rows](int index) {
        const QueueRowData *data = rows.rowAt(index);
        return data != nullptr ? data->title : QString{};
    };
    const auto start = [&](int count, const QVariantList &selected, int bridgeSelected) {
        bridge->setProperty("queueLength", count);
        bridge->setProperty("selectedQueueIndex", bridgeSelected);
        load(count);
        QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
            Q_ARG(QVariant, QVariant(selected))));
        controller->setProperty("selectionAnchorIndex", bridgeSelected);
        QVERIFY(QMetaObject::invokeMethod(controller, "handleBridgeSnapshotUpdate"));
        takeCommands();
    };
    // What the backend sends back for a removal, wherever it came from: the
    // selection it remapped, in place before the rows change as
    // BridgeClient does, then the removed ranges last first.
    const auto backendRemove = [&](QList<int> removed) {
        std::sort(removed.begin(), removed.end());
        int length = bridge->property("queueLength").toInt();
        int selected = bridge->property("selectedQueueIndex").toInt();
        if (selected >= 0) {
            const auto below = std::lower_bound(removed.begin(), removed.end(), selected);
            selected -= static_cast<int>(below - removed.begin());
            const int remaining = length - static_cast<int>(removed.size());
            selected = remaining > 0 ? std::min(selected, remaining - 1) : -1;
        }
        bridge->setProperty("selectedQueueIndex", selected);
        for (qsizetype end = removed.size(); end > 0;) {
            qsizetype first = end - 1;
            while (first > 0 && removed[first - 1] == removed[first] - 1) {
                --first;
            }
            const int count = static_cast<int>(end - first);
            length -= count;
            bridge->setProperty("queueLength", length);
            QVERIFY(rows.removeRowsAt(removed[first], count));
            end = first;
        }
    };
    const auto snapshotArrives = [controller]() {
        QVERIFY(QMetaObject::invokeMethod(controller, "handleBridgeSnapshotUpdate"));
    };

    // Deleting the highlighted row highlights the row that moves into its
    // place, the one the backend now has selected.
    start(5, {1}, 1);
    call("pressDelete");
    QCOMPARE(takeCommands(), QStringLiteral("remove 1"));
    // The row stays highlighted until the removal arrives.
    QCOMPARE(selection(), (QList<int>{1}));
    backendRemove({1});
    QCOMPARE(selection(), (QList<int>{1}));
    QCOMPARE(titleAt(1), QStringLiteral("Track 2"));
    // Before the snapshot's notification, Enter and Delete already act on
    // the highlighted row.
    call("pressEnter");
    QCOMPARE(takeCommands(), QStringLiteral("play 1"));
    snapshotArrives();
    QCOMPARE(takeCommands(), QString{});
    QCOMPARE(selection(), (QList<int>{1}));
    call("pressDelete");
    QCOMPARE(takeCommands(), QStringLiteral("remove 1"));
    backendRemove({1});
    snapshotArrives();
    QCOMPARE(selection(), (QList<int>{1}));
    QCOMPARE(titleAt(1), QStringLiteral("Track 3"));
    QCOMPARE(bridge->property("selectedQueueIndex").toInt(), 1);
    QCOMPARE(takeCommands(), QString{});

    // Deleting the last row highlights the new last row.
    start(3, {2}, 2);
    call("pressDelete");
    backendRemove({2});
    QCOMPARE(selection(), (QList<int>{1}));
    snapshotArrives();
    QCOMPARE(takeCommands(), QStringLiteral("remove 2"));
    QCOMPARE(bridge->property("selectedQueueIndex").toInt(), 1);

    // Deleting a multi-selection highlights the row that moves into the
    // place of the first one, and the backend is told so.
    start(6, {1, 3}, 3);
    call("pressDelete");
    QCOMPARE(takeCommands(), QStringLiteral("remove 3,1"));
    backendRemove({1, 3});
    QCOMPARE(selection(), (QList<int>{1}));
    QCOMPARE(titleAt(1), QStringLiteral("Track 2"));
    call("pressEnter");
    QCOMPARE(takeCommands(), QStringLiteral("play 1"));
    snapshotArrives();
    QCOMPARE(takeCommands(), QStringLiteral("select 1"));

    // A removal from elsewhere of one row of a multi-selection keeps the
    // rest. The backend's selection was that row, so it moved to a row
    // that is not highlighted; moving acts on the highlighted rows anyway.
    start(10, {2, 5, 7}, 5);
    backendRemove({5});
    QCOMPARE(selection(), (QList<int>{2, 6}));
    QCOMPARE(bridge->property("selectedQueueIndex").toInt(), 5);
    QVERIFY(QMetaObject::invokeMethod(controller, "moveSelected", Q_ARG(QVariant, 1)));
    QCOMPARE(takeCommands(), QStringLiteral("shift 2,6 by 1"));
    snapshotArrives();
    QCOMPARE(takeCommands(), QStringLiteral("select 6"));
    QCOMPARE(selection(), (QList<int>{2, 6}));
    call("pressDelete");
    QCOMPARE(takeCommands(), QStringLiteral("remove 6,2"));

    // A removal elsewhere keeps the selection on its rows.
    start(6, {3}, 3);
    backendRemove({0});
    QCOMPARE(selection(), (QList<int>{2}));
    QCOMPARE(titleAt(2), QStringLiteral("Track 3"));
    snapshotArrives();
    QCOMPARE(takeCommands(), QString{});
    QCOMPARE(bridge->property("selectedQueueIndex").toInt(), 2);

    // A move from elsewhere carries the highlighted row and the backend's
    // selection along together.
    start(6, {1}, 1);
    bridge->setProperty("selectedQueueIndex", 4);
    QVERIFY(rows.moveRow(1, 4));
    QCOMPARE(selection(), (QList<int>{4}));
    snapshotArrives();
    QCOMPARE(takeCommands(), QString{});
    call("pressEnter");
    QCOMPARE(takeCommands(), QStringLiteral("play 4"));

    // With nothing highlighted, Delete and Enter do nothing, whatever the
    // backend last had selected.
    start(4, {}, 2);
    QCOMPARE(selection(), QList<int>{});
    call("pressDelete");
    call("pressEnter");
    QVERIFY(QMetaObject::invokeMethod(controller, "moveSelected", Q_ARG(QVariant, 1)));
    QCOMPARE(takeCommands(), QString{});
}

void LibraryQueueSmokeTest::queueAutoCenterIsDeferredOffHandlerStack() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness
    property bool positionViewCalled: false
    property int positionViewIndex: -1

    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property string currentTrackPath: "/music/old.flac"
        property int playingQueueIndex: 5
        property int queueLength: 100
        property int selectedQueueIndex: -1
        property bool profileLogsEnabled: false
    }

    QtObject {
        id: stubView
        objectName: "stubView"
        property bool visible: true
        property real rowHeight: 24
        property real height: 400
        property real contentY: 0
        property real contentHeight: 2400
    }

    Controllers.QueueController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(sel) { return false } }
        openTagEditorDialog: function() {}
    }

    function triggerTrackChange() {
        bridge.currentTrackPath = "/music/new.flac"
        bridge.playingQueueIndex = 42
        controller.handleSnapshotChanged(stubView)
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    // Initialize controller so it has a lastAutoCenterTrackPath to compare against.
    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));

    // Trigger a track change — handleSnapshotChanged detects path changed
    // and should defer scroll via a 0ms Timer.
    QObject *stubView = root->findChild<QObject *>(QStringLiteral("stubView"));
    QVERIFY(stubView != nullptr);
    QCOMPARE(stubView->property("contentY").toDouble(), 0.0);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "triggerTrackChange"));

    // Immediately after handler returns: contentY must NOT have changed yet.
    QCOMPARE(stubView->property("contentY").toDouble(), 0.0);

    // Process the event loop so the 0ms Timer fires.
    // Index 42 * 24px row height = 1008px row top. Since row is below
    // the viewport (400px), contentY should be set to rowBottom - viewHeight
    // = 1008 + 24 - 400 = 632.
    QTRY_VERIFY(stubView->property("contentY").toDouble() > 0.0);
    QCOMPARE(stubView->property("contentY").toDouble(), 632.0);
}

// Helper: create a QueueController + stub view and invoke _containIndex.
// Returns the resulting contentY.
static double invokeContainIndex(
    QQmlApplicationEngine &engine,
    const QUrl &baseUrl,
    double initialContentY,
    double viewHeight,
    double contentHeight,
    int targetIndex)
{
    QString errorText;
    const QByteArray qml = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness

    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property string currentTrackPath: "/music/track.flac"
        property int playingQueueIndex: 0
        property int queueLength: 1000
        property int selectedQueueIndex: -1
        property bool profileLogsEnabled: false
    }

    QtObject {
        id: stubView
        objectName: "stubView"
        property bool visible: true
        property real rowHeight: 24
        property real height: 400
        property real contentY: 0
        property real contentHeight: 24000
    }

    Controllers.QueueController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(sel) { return false } }
        openTagEditorDialog: function() {}
    }

    function callContainIndex(index) {
        controller._containIndex(stubView, index)
    }
}
)QML");
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, qml, baseUrl, &errorText));
    if (!root) {
        qWarning("invokeContainIndex: %s", qPrintable(errorText));
        return -1.0;
    }
    QObject *view = root->findChild<QObject *>(QStringLiteral("stubView"));
    view->setProperty("contentY", initialContentY);
    view->setProperty("height", viewHeight);
    view->setProperty("contentHeight", contentHeight);
    QMetaObject::invokeMethod(root.data(), "callContainIndex",
        Q_ARG(QVariant, targetIndex));
    return view->property("contentY").toDouble();
}

void LibraryQueueSmokeTest::queueContainIndexSkipsScrollWhenVisible() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 5 → rowTop = 120, rowBottom = 144.
    // Viewport: contentY=100, height=400 → visible range [100, 500].
    // Row is fully inside viewport → contentY must not change.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/100.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/5);
    QCOMPARE(result, 100.0);
}

void LibraryQueueSmokeTest::queueContainIndexScrollsUpWhenAboveViewport() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 2 → rowTop = 48.
    // Viewport: contentY=200, height=400 → visible range [200, 600].
    // Row is above viewport → contentY should snap to rowTop = 48.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/200.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/2);
    QCOMPARE(result, 48.0);
}

void LibraryQueueSmokeTest::queueContainIndexClampsAtListEnd() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 999 → rowTop = 23976, rowBottom = 24000.
    // contentHeight = 24000, viewHeight = 400.
    // maxY = 24000 - 400 = 23600.
    // Target would be rowBottom - viewHeight = 24000 - 400 = 23600.
    // Clamped to maxY = 23600.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/0.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/999);
    QCOMPARE(result, 23600.0);
}

namespace {

QVariantList sampleQueueRows(int count) {
    QVariantList rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(QVariantMap{
            {QStringLiteral("title"), QStringLiteral("Track %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist")},
            {QStringLiteral("album"), QStringLiteral("Album")},
            {QStringLiteral("lengthText"), QStringLiteral("1:00")},
        });
    }
    return rows;
}

int callIntFunction(QObject *object, const char *function) {
    QVariant result;
    if (!QMetaObject::invokeMethod(object, function, Q_RETURN_ARG(QVariant, result))) {
        return -2;
    }
    return result.toInt();
}

} // namespace

void LibraryQueueSmokeTest::wheelScrollsListsByTheWheelDelta() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("queueRows", sampleQueueRows(300));
    auto *view = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(view != nullptr);
    QTRY_VERIFY(view->property("contentHeight").toReal() > view->height() * 3);
    const QPointF center = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
    const auto contentY = [view]() { return view->property("contentY").toReal(); };
    // The rows' height follows the font size.
    const qreal rowHeight = view->property("rowHeight").toReal();
    QVERIFY(rowHeight > 0);

    // A touchpad reports the pixels to scroll, along with an angle that
    // is a small part of a notch. The list scrolls by the pixels.
    qreal before = contentY();
    QVERIFY(sendWheel(window, center, QPoint(0, -3), QPoint(0, -24)));
    QCOMPARE(contentY() - before, 3.0);

    // A high-resolution wheel reports an eighth of a notch; a notch
    // scrolls three rows.
    before = contentY();
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, -15)));
    QVERIFY2(std::abs(contentY() - before - 3 * rowHeight / 8) <= 1.0,
             qPrintable(QStringLiteral("scrolled %1").arg(contentY() - before)));

    // Steps smaller than a pixel add up instead of being rounded away.
    before = contentY();
    for (int i = 0; i < 10; ++i) {
        QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, -2)));
    }
    QVERIFY2(std::abs(contentY() - before - rowHeight / 2) <= 1.0,
             qPrintable(QStringLiteral("scrolled %1").arg(contentY() - before)));

    // Scrolling back up works the same way.
    before = contentY();
    QVERIFY(sendWheel(window, center, QPoint(0, 5), QPoint(0, 40)));
    QVERIFY(std::abs(before - contentY() - 5.0) <= 1.0);

    // Horizontal scrolling does not scroll the list.
    before = contentY();
    sendWheel(window, center, QPoint(), QPoint(-120, 0));
    QCOMPARE(contentY(), before);
}

void LibraryQueueSmokeTest::queueKeysReachBothEndsAndOpenTheContextMenu() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(300));
    bridge->setProperty("queueLength", 300);
    QObject *controller = window->findChild<QObject *>(QStringLiteral("queueController"));
    auto *view = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(controller != nullptr && view != nullptr);
    QTRY_COMPARE(bridge->property("queueLength").toInt(), 300);
    QVERIFY(QMetaObject::invokeMethod(view, "forceActiveFocus"));
    QTRY_VERIFY(view->hasActiveFocus());

    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_End);
    QTRY_COMPARE(callIntFunction(controller, "primarySelectedIndex"), 299);
    QTest::keyClick(window, Qt::Key_Home);
    QTRY_COMPARE(callIntFunction(controller, "primarySelectedIndex"), 0);

    QVERIFY(!anyMenuVisible(window, QStringLiteral("playlistContextMenu")));
    QTest::keyClick(window, Qt::Key_Menu);
    QTRY_VERIFY(anyMenuVisible(window, QStringLiteral("playlistContextMenu")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!anyMenuVisible(window, QStringLiteral("playlistContextMenu")));
    QTRY_VERIFY(view->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
    QTRY_VERIFY(anyMenuVisible(window, QStringLiteral("playlistContextMenu")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!anyMenuVisible(window, QStringLiteral("playlistContextMenu")));
}

void LibraryQueueSmokeTest::libraryKeysReachBothEndsAndOpenTheContextMenu() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    libraryModel.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    libraryModel.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());
    QTRY_COMPARE(libraryModel.rowCount(), 3);
    auto *libraryView = qvariant_cast<QQuickItem *>(window->property("libraryViewRef"));
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(libraryView != nullptr && library != nullptr);
    QVERIFY(QMetaObject::invokeMethod(libraryView, "forceActiveFocus"));
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(library, "selectIndex", Q_ARG(QVariant, 0)));
    const auto current = [library]() {
        return callIntFunction(library, "currentSelectionIndex");
    };
    // The rows share one context menu, which every row opens.
    const auto rowMenuVisible = [window]() {
        return anyMenuVisible(window, QStringLiteral("libraryContextMenu"));
    };
    const auto selectedCount = [&libraryModel]() {
        return libraryModel.selectionCount();
    };

    QTest::keyClick(window, Qt::Key_End);
    QTRY_COMPARE(current(), 2);
    QTest::keyClick(window, Qt::Key_Home);
    QTRY_COMPARE(current(), 0);
    // A page is more than these three rows.
    QTest::keyClick(window, Qt::Key_PageDown);
    QTRY_COMPARE(current(), 2);
    QTest::keyClick(window, Qt::Key_PageUp);
    QTRY_COMPARE(current(), 0);
    QTest::keyClick(window, Qt::Key_End, Qt::ShiftModifier);
    QTRY_COMPARE(selectedCount(), 3);
    QTest::keyClick(window, Qt::Key_Home);
    QTRY_COMPARE(selectedCount(), 1);

    QVERIFY(!rowMenuVisible());
    QTest::keyClick(window, Qt::Key_Menu);
    QTRY_VERIFY(rowMenuVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!rowMenuVisible());
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
    QTRY_VERIFY(rowMenuVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!rowMenuVisible());
}

namespace {

// Artists with two albums each, one with a year and one without, and
// `tracksPerAlbum` tracks in each: the delegates the view reuses go from
// rows with a year to rows without one.
QByteArray artistsWithDatedAndUndatedAlbumsBinary(int artistCount, int tracksPerAlbum) {
    QVector<BinaryTreeRow> rows;
    for (int artist = 1; artist <= artistCount; ++artist) {
        const QString name = QStringLiteral("Artist %1").arg(artist, 2, 10, QLatin1Char('0'));
        const QString artistPath = QStringLiteral("/music/") + name;
        rows.push_back(BinaryTreeRow{1, 0, -1, 0, 2, name + QStringLiteral(" (2)"),
            QStringLiteral("artist|%1").arg(name), name, artistPath, {}, {}, {}});
        for (int album = 1; album <= 2; ++album) {
            // A long title, which a narrow pane elides before the year.
            const QString albumName = QStringLiteral("Album %1 With A Title Long Enough To Elide").arg(album);
            const QString albumPath = artistPath + QStringLiteral("/Album %1").arg(album);
            BinaryTreeRow albumRow{2, 1, -1, 0, static_cast<quint32>(tracksPerAlbum), albumName,
                QStringLiteral("album|%1|%2").arg(name, albumName), name, albumPath, {}, {}, {}};
            albumRow.year = album == 1 ? 1980 + artist : 0;
            rows.push_back(albumRow);
            for (int track = 1; track <= tracksPerAlbum; ++track) {
                const QString path = albumPath + QStringLiteral("/%1.flac").arg(track, 2, 10, QLatin1Char('0'));
                rows.push_back(BinaryTreeRow{4, 2, -1, static_cast<quint32>(track), 0,
                    QStringLiteral("%1 - Track").arg(track, 2, 10, QLatin1Char('0')),
                    QStringLiteral("track|%1").arg(path), name, path, {}, path, QStringList{path}});
            }
        }
    }
    return encodeRows(rows);
}

QStringList datedAndUndatedAlbumKeys(int artistCount) {
    QStringList keys;
    for (int artist = 1; artist <= artistCount; ++artist) {
        const QString name = QStringLiteral("Artist %1").arg(artist, 2, 10, QLatin1Char('0'));
        keys.push_back(QStringLiteral("artist|%1").arg(name));
        for (int album = 1; album <= 2; ++album) {
            keys.push_back(QStringLiteral("album|%1|Album %2 With A Title Long Enough To Elide").arg(name).arg(album));
        }
    }
    return keys;
}

} // namespace

// The rows lay out their years and titles without a QML warning, such as a
// binding loop, with and without a year, collapsed and expanded, and in a
// narrow pane (#474). The year stays right-aligned at the scrollbar lane,
// and the title ends before it.
void LibraryQueueSmokeTest::libraryRowsLayOutTheirYearsWithoutWarnings() {
    constexpr int artistCount = 24;
    constexpr int tracksPerAlbum = 3;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    auto *libraryView = qvariant_cast<QQuickItem *>(window->property("libraryViewRef"));
    QVERIFY(libraryView != nullptr);
    QVERIFY2(takeCapturedMessagesText().isEmpty(), "the window warned before the rows came");

    // The pane's warnings so far, with the phase that caused them.
    QStringList warnings;
    const auto collect = [&warnings](const char *phase) {
        QStringList lines = takeCapturedMessagesText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        lines.removeDuplicates();
        if (!lines.isEmpty()) {
            warnings << QStringLiteral("%1: %2").arg(QLatin1String(phase), lines.join(QLatin1Char('\n')));
        }
    };
    // The next frame: the view lays out its rows when the window polishes.
    QSignalSpy frames(window, &QQuickWindow::frameSwapped);
    const auto settle = [window, &frames]() {
        frames.clear();
        window->update();
        QVERIFY(frames.wait(5000));
    };
    // Every row from top to bottom, and back, so the view reuses its
    // delegates for rows of each kind.
    const auto scrollThrough = [&]() {
        const qreal step = std::max<qreal>(1.0, libraryView->height() / 2.0);
        for (qreal y = 0.0;; y += step) {
            const qreal end = libraryView->property("contentHeight").toReal() - libraryView->height();
            libraryView->setProperty("contentY", std::min(y, std::max<qreal>(0.0, end)));
            settle();
            if (y >= end) {
                break;
            }
        }
        libraryView->setProperty("contentY", 0.0);
        settle();
    };
    // The visible rows with a year: every year ends the same distance,
    // the scrollbar lane and the inset, before its row's right end, and its
    // title ends before it.
    qreal yearRightGap = -1.0;
    const auto checkRowGeometry = [libraryView, &yearRightGap]() -> QString {
        const qreal rowInset = 12.0;
        int dated = 0;
        for (QQuickItem *row : visibleItemsNamed(libraryView, QStringLiteral("libraryRow"))) {
            QQuickItem *year = nullptr;
            QQuickItem *title = nullptr;
            for (QQuickItem *child : row->childItems()) {
                if (child->objectName() == QLatin1String("libraryRowYear")) {
                    year = child;
                } else if (child->objectName() == QLatin1String("libraryRowTitle")) {
                    title = child;
                }
            }
            if (year == nullptr || title == nullptr) {
                return QStringLiteral("a row without its year or title");
            }
            const QString yearText = year->property("text").toString();
            if (!year->isVisible()) {
                // A row without a year: its title reaches the year's place.
                if (!yearText.isEmpty()) {
                    return QStringLiteral("a hidden year with text %1").arg(yearText);
                }
                continue;
            }
            ++dated;
            if (yearText.size() != 4 || year->width() < 1.0) {
                return QStringLiteral("a year %1 of width %2").arg(yearText).arg(year->width());
            }
            const qreal gap = row->width() - (year->x() + year->width());
            if (yearRightGap < 0.0) {
                yearRightGap = gap;
            }
            if (gap <= rowInset || !qFuzzyCompare(gap, yearRightGap)) {
                return QStringLiteral("year %1 ends %2 px before its row's end, not %3")
                    .arg(yearText).arg(gap).arg(yearRightGap);
            }
            if (title->x() + title->width() > year->x()) {
                return QStringLiteral("a title overlapping year %1").arg(yearText);
            }
        }
        return dated > 0 ? QString() : QStringLiteral("no row with a year");
    };

    libraryModel.setExpandedKeys(datedAndUndatedAlbumKeys(artistCount));
    QVERIFY(applyLibraryTree(libraryModel, artistsWithDatedAndUndatedAlbumsBinary(artistCount, tracksPerAlbum)));
    QTRY_COMPARE(libraryModel.rowCount(), artistCount * (1 + 2 * (1 + tracksPerAlbum)));
    settle();
    collect("expanded");
    QCOMPARE(checkRowGeometry(), QString());
    scrollThrough();
    collect("scrolled expanded");

    // Collapsed albums: every album row shows, with and without a year.
    QStringList artistsOnly;
    for (int artist = 1; artist <= artistCount; ++artist) {
        artistsOnly << QStringLiteral("artist|Artist %1").arg(artist, 2, 10, QLatin1Char('0'));
    }
    libraryModel.setExpandedKeys(artistsOnly);
    QTRY_COMPARE(libraryModel.rowCount(), artistCount * 3);
    settle();
    collect("albums collapsed");
    QCOMPARE(checkRowGeometry(), QString());
    scrollThrough();
    collect("scrolled collapsed");

    // A narrow pane elides the titles before their years.
    const int wideWidth = window->width();
    window->resize(480, window->height());
    settle();
    collect("narrow");
    QCOMPARE(checkRowGeometry(), QString());
    const QList<QQuickItem *> titles = visibleItemsNamed(libraryView, QStringLiteral("libraryRowTitle"));
    QVERIFY(std::any_of(titles.cbegin(), titles.cend(), [](QQuickItem *title) {
        return title->property("truncated").toBool();
    }));
    scrollThrough();
    collect("scrolled narrow");

    // Expanded again in the narrow pane, then wide again.
    libraryModel.setExpandedKeys(datedAndUndatedAlbumKeys(artistCount));
    QTRY_COMPARE(libraryModel.rowCount(), artistCount * (1 + 2 * (1 + tracksPerAlbum)));
    settle();
    collect("expanded narrow");
    QCOMPARE(checkRowGeometry(), QString());
    scrollThrough();
    collect("scrolled expanded narrow");
    window->resize(wideWidth, window->height());
    settle();
    collect("wide again");
    QCOMPARE(checkRowGeometry(), QString());

    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
}

namespace {

QStringList queueCommands(QObject *bridge) {
    return bridge->property("queueCommands").toStringList();
}

} // namespace

void LibraryQueueSmokeTest::deleteRemovesQueueTracksOnlyWhileTheQueueHasFocus() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(5));
    bridge->setProperty("queueLength", 5);
    QObject *controller = window->findChild<QObject *>(QStringLiteral("queueController"));
    auto *queueView = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    auto *libraryView = qvariant_cast<QQuickItem *>(window->property("libraryViewRef"));
    QVERIFY(controller != nullptr && queueView != nullptr && libraryView != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 1)));

    // The library has focus, as after a click on one of its rows: Delete
    // leaves the queue alone, also with every queue row highlighted.
    QVERIFY(QMetaObject::invokeMethod(libraryView, "forceActiveFocus"));
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Delete);
    QVERIFY(QMetaObject::invokeMethod(controller, "selectAllItems"));
    QTest::keyClick(window, Qt::Key_Delete);
    QCoreApplication::processEvents();
    QCOMPARE(queueCommands(bridge), QStringList{});

    // So does a text field.
    QObject *search = nullptr;
    for (QObject *action : window->findChildren<QObject *>()) {
        if (action->property("text").toString() == QStringLiteral("&Find…")) {
            search = action;
            break;
        }
    }
    QVERIFY(search != nullptr);
    QVERIFY(QMetaObject::invokeMethod(search, "trigger"));
    auto *field = window->findChild<QQuickItem *>(QStringLiteral("globalSearchQueryField"));
    QVERIFY(field != nullptr);
    QTRY_VERIFY(field->hasActiveFocus());
    QTest::keyClick(window, 'a');
    QTest::keyClick(window, Qt::Key_Home);
    QTest::keyClick(window, Qt::Key_Delete);
    QTRY_COMPARE(field->property("text").toString(), QString());
    QTest::keyClick(window, Qt::Key_Escape);
    QCOMPARE(queueCommands(bridge), QStringList{});

    // With the queue focused, Delete removes the highlighted rows.
    QVERIFY(QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 1)));
    QVERIFY(QMetaObject::invokeMethod(queueView, "forceActiveFocus"));
    QTRY_VERIFY(queueView->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Delete);
    QCOMPARE(queueCommands(bridge), QStringList{QStringLiteral("remove 1")});
    // No action takes Delete from the window: the queue's context menu
    // shows it beside Remove from Queue without taking it.
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->inherits("QQuickAction")) {
            QVERIFY2(object->property("shortcut").value<QKeySequence>() != QKeySequence(QKeySequence::Delete),
                     qPrintable(object->property("text").toString()));
        }
    }
    QObject *remove = window->findChild<QObject *>(QStringLiteral("queueRemoveItem"));
    QVERIFY(remove != nullptr);
    QVERIFY(qvariant_cast<QObject *>(remove->property("action")) == nullptr);
}

void LibraryQueueSmokeTest::queueChangesShowNoNoticeAndEditUndoRestoresThem() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(5));
    bridge->setProperty("queueLength", 5);
    QObject *controller = window->findChild<QObject *>(QStringLiteral("queueController"));
    QObject *undoAction = window->findChild<QObject *>(QStringLiteral("undoQueueChangeAction"));
    auto *queueView = window->findChild<QQuickItem *>(QStringLiteral("playlistView"));
    QVERIFY(controller != nullptr && undoAction != nullptr && queueView != nullptr);
    QObject *undoMenuItem = nullptr;
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->inherits("QQuickMenuItem")
            && qvariant_cast<QObject *>(object->property("action")) == undoAction) {
            undoMenuItem = object;
            break;
        }
    }
    QVERIFY(undoMenuItem != nullptr);
    QVERIFY(!undoMenuItem->property("enabled").toBool());
    QCOMPARE(undoMenuItem->property("text").toString(), QStringLiteral("&Undo"));

    // The queue keeps its height: nothing takes its place for a change.
    const qreal queueHeight = queueView->height();
    QVERIFY(queueHeight > 0);
    const auto expectNoNotice = [&]() {
        QVERIFY(window->findChild<QObject *>(QStringLiteral("queueUndoNotice")) == nullptr);
        QVERIFY(window->findChild<QObject *>(QStringLiteral("queueUndoButton")) == nullptr);
        QVERIFY(window->findChild<QObject *>(QStringLiteral("queueUndoDismissButton")) == nullptr);
        QCOMPARE(queueView->height(), queueHeight);
    };
    const auto offerUndo = [&](int kind, double id) {
        bridge->setProperty("queueUndoKind", kind);
        bridge->setProperty("queueUndoId", id);
    };
    const auto takeBackOffer = [&]() {
        bridge->setProperty("queueUndoKind", 0);
        bridge->setProperty("queueUndoId", 0.0);
    };

    QVERIFY(QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 1)));
    QVERIFY(QMetaObject::invokeMethod(controller, "toggleSelection", Q_ARG(QVariant, 3)));
    QVERIFY(QMetaObject::invokeMethod(queueView, "forceActiveFocus"));
    QTRY_VERIFY(queueView->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Delete);
    QCOMPARE(queueCommands(bridge), QStringList{QStringLiteral("remove 3,1")});
    // The backend removed the rows and offers to undo it.
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    offerUndo(2, 7.0);
    expectNoNotice();
    QVERIFY(undoMenuItem->property("enabled").toBool());
    QCOMPARE(undoMenuItem->property("text").toString(), QStringLiteral("&Undo Remove from Queue"));
    QVERIFY(QMetaObject::invokeMethod(undoAction, "trigger"));
    QCOMPARE(queueCommands(bridge).last(), QStringLiteral("undo 7"));
    // The rows come back, and with them their highlight.
    bridge->setProperty("queueRows", sampleQueueRows(5));
    bridge->setProperty("queueLength", 5);
    takeBackOffer();
    QVERIFY(QMetaObject::invokeMethod(bridge, "queueUndoRestored",
        Q_ARG(QVariant, QVariant(QVariantList{1, 3}))));
    QCOMPARE(controller->property("selectedIndices").toList(), (QVariantList{1, 3}));
    QVERIFY(!undoMenuItem->property("enabled").toBool());

    // Clear Playlist, from the menu and from Ctrl+Z.
    offerUndo(1, 8.0);
    expectNoNotice();
    QVERIFY(undoMenuItem->property("enabled").toBool());
    QCOMPARE(undoMenuItem->property("text").toString(), QStringLiteral("&Undo Clear Queue"));
    QVERIFY(QMetaObject::invokeMethod(undoAction, "trigger"));
    QCOMPARE(queueCommands(bridge).last(), QStringLiteral("undo 8"));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(queueCommands(bridge).last(), QStringLiteral("undo 8"));
    QCOMPARE(queueCommands(bridge).count(QStringLiteral("undo 8")), 2);

    // A replaced queue, from the menu and from Ctrl+Z.
    offerUndo(3, 9.0);
    expectNoNotice();
    QVERIFY(undoMenuItem->property("enabled").toBool());
    QCOMPARE(undoMenuItem->property("text").toString(), QStringLiteral("&Undo Replace Queue"));
    QVERIFY(QMetaObject::invokeMethod(undoAction, "trigger"));
    QCOMPARE(queueCommands(bridge).last(), QStringLiteral("undo 9"));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(queueCommands(bridge).count(QStringLiteral("undo 9")), 2);

    // Moved rows go back the same way.
    offerUndo(4, 10.0);
    expectNoNotice();
    QVERIFY(undoMenuItem->property("enabled").toBool());
    QCOMPARE(undoMenuItem->property("text").toString(), QStringLiteral("&Undo Move in Queue"));
    QVERIFY(QMetaObject::invokeMethod(queueView, "forceActiveFocus"));
    QTRY_VERIFY(queueView->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(queueCommands(bridge).last(), QStringLiteral("undo 10"));
}

void LibraryQueueSmokeTest::editMenuNamesTheUndoOfAMoveAndItSelectsTheMovedRows() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    bridge->setProperty("queueRows", sampleQueueRows(5));
    bridge->setProperty("queueLength", 5);
    QObject *controller = window->findChild<QObject *>(QStringLiteral("queueController"));
    QObject *undoAction = window->findChild<QObject *>(QStringLiteral("undoQueueChangeAction"));
    QVERIFY(controller != nullptr && undoAction != nullptr);
    QObject *undoMenuItem = nullptr;
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->inherits("QQuickMenuItem")
            && qvariant_cast<QObject *>(object->property("action")) == undoAction) {
            undoMenuItem = object;
            break;
        }
    }
    QVERIFY(undoMenuItem != nullptr);

    bridge->setProperty("queueUndoKind", int(BinaryBridgeCodec::QueueUndoMoved));
    bridge->setProperty("queueUndoId", 10.0);
    QVERIFY(undoMenuItem->property("enabled").toBool());
    QCOMPARE(undoMenuItem->property("text").toString(), QStringLiteral("&Undo Move in Queue"));
    QVERIFY(QMetaObject::invokeMethod(undoAction, "trigger"));
    QCOMPARE(queueCommands(bridge).last(), QStringLiteral("undo 10"));

    // The rows are back where they were, and selected.
    bridge->setProperty("queueUndoKind", int(BinaryBridgeCodec::QueueUndoNone));
    bridge->setProperty("queueUndoId", 0.0);
    QVERIFY(QMetaObject::invokeMethod(bridge, "queueUndoRestored",
        Q_ARG(QVariant, QVariant(QVariantList{0, 2}))));
    QCOMPARE(controller->property("selectedIndices").toList(), (QVariantList{0, 2}));
    QVERIFY(!undoMenuItem->property("enabled").toBool());
    QCOMPARE(undoMenuItem->property("text").toString(), QStringLiteral("&Undo"));
}

void LibraryQueueSmokeTest::removingALibraryRootAsksFirstAndNamesIt() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "preferences" as Preferences
ApplicationWindow {
    id: window
    width: 900
    height: 700
    visible: true
    property var removed: []
    Components.UiPalette { id: palette; windowRoot: window }
    QtObject {
        id: bridge
        property bool libraryWatchEnabled: true
        property var libraryRootEntries: [{
            path: "/fixture/Music/Jazz", name: "Jazz", displayName: "Jazz",
            unwatchedReason: "", trackCount: 1234
        }]
        function removeLibraryRoot(path) { window.removed = window.removed.concat([path]) }
        function setLibraryWatchEnabled(value) {}
        function openInFileBrowser(path) {}
        function rescanLibraryRoot(path) {}
        function rescanAllLibraryRoots() {}
    }
    QtObject {
        id: settings
        property int librarySortMode: 0
        function value(key) { return key === "librarySortMode" ? librarySortMode : bridge[key] }
        function setValue(key, value) {}
    }
    Preferences.LibraryPage {
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
        settings: settings
        windowRoot: window
        promptAddLibraryRoot: function() {}
        openLibraryRootNameDialog: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *confirm = root->findChild<QObject *>(QStringLiteral("removeLibraryRootConfirmDialog"));
    QVERIFY(confirm != nullptr);

    // Remove acts on the selected folder: none at first, then the one
    // clicked (#373 H4).
    QQuickItem *removeButton = visibleItemNamed(window, QStringLiteral("libraryRootRemoveButton"));
    QVERIFY(removeButton != nullptr);
    QVERIFY(!removeButton->isEnabled());
    QQuickItem *folder = visibleItemNamed(window, QStringLiteral("libraryRootRow"));
    QVERIFY(folder != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(folder));
    QTRY_VERIFY(removeButton->isEnabled());
    QVERIFY(clickVisibleButton(window, QStringLiteral("libraryRootRemoveButton")));
    QTRY_VERIFY(confirm->property("opened").toBool());
    // A window of its own above the page's (#373 H5).
    auto *confirmWindow = qobject_cast<QQuickWindow *>(confirm);
    QVERIFY(confirmWindow != nullptr);
    QCOMPARE(confirmWindow->transientParent(), window);
    QVERIFY(QTest::qWaitForWindowExposed(confirmWindow));
    QCOMPARE(root->property("removed").toStringList(), QStringList{});
    const QString message = confirm->property("message").toString();
    QVERIFY2(message.contains(QStringLiteral("Jazz")), qPrintable(message));
    // The path on a line of its own, elided in the middle, whole in its
    // tooltip.
    QCOMPARE(confirm->property("path").toString(), QStringLiteral("/fixture/Music/Jazz"));
    QQuickItem *path = visibleItemNamed(confirmWindow, QStringLiteral("confirmDialogPath"));
    QVERIFY(path != nullptr);
    QCOMPARE(path->property("text").toString(), QStringLiteral("/fixture/Music/Jazz"));
    QCOMPARE(path->property("elide").toInt(), int(Qt::ElideMiddle));
    QCOMPARE(path->property("lineCount").toInt(), 1);
    const QString details = confirm->property("details").toString();
    // Counts are written as the locale writes numbers.
    QVERIFY2(details.contains(QLocale().toString(1234) + QStringLiteral(" tracks"))
                 && details.contains(QStringLiteral("stay on disk")),
             qPrintable(details));

    QVERIFY(clickVisibleButton(confirmWindow, QStringLiteral("confirmDialogCancelButton")));
    QTRY_VERIFY(!confirm->property("visible").toBool());
    QCOMPARE(root->property("removed").toStringList(), QStringList{});

    QVERIFY(clickVisibleButton(window, QStringLiteral("libraryRootRemoveButton")));
    QTRY_VERIFY(confirm->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(confirmWindow));
    // A quick second click finds the window gone: the folder is removed
    // once.
    QQuickItem *confirmButton = visibleItemNamed(confirmWindow, QStringLiteral("confirmDialogConfirmButton"));
    QVERIFY(confirmButton != nullptr);
    const QPoint confirmCenter = centerInWindow(confirmButton);
    QTest::mouseClick(confirmWindow, Qt::LeftButton, Qt::NoModifier, confirmCenter);
    QVERIFY(!confirmWindow->isVisible());
    QTest::mouseClick(confirmWindow, Qt::LeftButton, Qt::NoModifier, confirmCenter);
    QTRY_COMPARE(root->property("removed").toStringList(),
                 QStringList{QStringLiteral("/fixture/Music/Jazz")});
    QCoreApplication::processEvents();
    QCOMPARE(root->property("removed").toStringList(), QStringList{QStringLiteral("/fixture/Music/Jazz")});
}

namespace {

// The library controller, with a bridge that records what it is asked to
// queue, over `testLibraryModel`.
const QByteArray kLibraryControllerHarness = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness
    width: 320
    height: 240
    property alias controllerRef: controller
    property alias viewRef: view
    property var played: []
    property var appended: []
    property var inserted: []
    property var insertedRows: []

    function clickRow(row, modifiers) {
        controller.handleRowSelection(
            row, testLibraryModel.rowDataForRow(row), Qt.LeftButton, modifiers)
    }
    function canPlay() { return controller.canPlaySelection() }
    function playSelection() { controller.playSelection() }
    function appendSelection() { controller.appendSelection() }
    function playRow(row) { controller.playRowItems(testLibraryModel.rowDataForRow(row)) }
    function insertRow(row, at) {
        controller.insertRowItems(testLibraryModel.rowDataForRow(row), at)
    }
    function rowSelectedInView(row) {
        const item = view.itemAtIndex(row)
        return item ? item.rowSelected : null
    }

    QtObject {
        id: bridge
        property int libraryTrackCount: 1
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
        function setLibraryNodeExpanded(key, expanded) {}
        function playLibraryItems(items) { harness.played = harness.played.concat([items]) }
        function appendLibraryItems(items) { harness.appended = harness.appended.concat([items]) }
        function insertLibraryItems(items, row) {
            harness.inserted = harness.inserted.concat([items])
            harness.insertedRows = harness.insertedRows.concat([row])
        }
        function openInFileBrowser(path) {}
    }
    QtObject { id: tagEditorApi }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    // As the library pane's rows: reused delegates that show the role.
    ListView {
        id: view
        readonly property real rowHeight: 24
        anchors.fill: parent
        model: testLibraryModel
        reuseItems: true
        delegate: Rectangle {
            readonly property bool rowSelected: !!model.selected
            width: ListView.view.width
            height: view.rowHeight
            color: rowSelected ? "blue" : "white"
        }
        Component.onCompleted: controller.registerView(view)
    }
}
)QML");

QObject *createLibraryControllerHarness(QQmlApplicationEngine &engine, LibraryTreeModel &model,
                                        QString *errorText) {
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    return createQmlObjectFromSource(engine, kLibraryControllerHarness, baseUrl, errorText);
}

void callWith(QObject *object, const char *method, const QVariant &first,
              const QVariant &second = {}) {
    if (second.isValid()) {
        QVERIFY(QMetaObject::invokeMethod(object, method, Q_ARG(QVariant, first),
                                          Q_ARG(QVariant, second)));
    } else {
        QVERIFY(QMetaObject::invokeMethod(object, method, Q_ARG(QVariant, first)));
    }
}

// The queue items each call of the harness bridge's `property` received.
QVector<BinaryBridgeCodec::LibraryQueueItems> recordedItems(QObject *harness, const char *property) {
    QVector<BinaryBridgeCodec::LibraryQueueItems> calls;
    for (const QVariant &call : harness->property(property).toList()) {
        calls.push_back(call.value<BinaryBridgeCodec::LibraryQueueItems>());
    }
    return calls;
}

BinaryBridgeCodec::LibraryQueueItem pathsItem(const QStringList &paths) {
    BinaryBridgeCodec::LibraryQueueItem item;
    item.paths = paths;
    return item;
}

} // namespace

void LibraryQueueSmokeTest::shiftSelectingTenThousandLibraryRowsStaysFast() {
    const QString root = QStringLiteral("/music");
    // 260 expanded artists of 38 tracks: 10,400 rows.
    const QVector<int> artists = artistRange(0, 259);
    LibraryTreeModel model;
    model.setExpandedKeys(expandedArtistAlbumKeys(root, artists));
    QVERIFY(applyLibraryTree(model, encodeRows(artistAlbumRows(root, artists, 38, plainTrackTitle))));
    QCOMPARE(model.rowCount(), 10400);

    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> harness(createLibraryControllerHarness(engine, model, &errorText));
    QVERIFY2(harness != nullptr, qPrintable(errorText));

    callWith(harness.get(), "clickRow", 0, int(Qt::NoModifier));
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QElapsedTimer timer;
    timer.start();
    callWith(harness.get(), "clickRow", 9999, int(Qt::ShiftModifier));
    const qint64 rangeMs = timer.elapsed();
    QVERIFY2(rangeMs < 2000, qPrintable(QStringLiteral("Shift-select took %1 ms").arg(rangeMs)));
    QCOMPARE(model.selectionCount(), 10000);
    QVERIFY(model.isRowSelected(9999));
    QVERIFY(!model.isRowSelected(10000));
    // Views hear of the rows that changed as a run, not one by one.
    QVERIFY2(changed.count() <= 2, qPrintable(QString::number(changed.count())));
    QCOMPARE(harness->property("controllerRef").value<QObject *>()
                 ->property("selectedSelectionKey").toString(),
             model.selectionKeyForRow(9999));
    QVariant shown;
    QVERIFY(QMetaObject::invokeMethod(harness.get(), "rowSelectedInView",
                                      Q_RETURN_ARG(QVariant, shown), Q_ARG(QVariant, 3)));
    QCOMPARE(shown.toBool(), true);

    // The actions' enabled checks ask the model, which keeps the answer.
    timer.restart();
    for (int i = 0; i < 200; ++i) {
        QVariant enabled;
        QVERIFY(QMetaObject::invokeMethod(harness.get(), "canPlay", Q_RETURN_ARG(QVariant, enabled)));
        QVERIFY(enabled.toBool());
    }
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));

    // A row's check costs the same however many rows are selected.
    const auto timeRowChecks = [&model]() {
        QElapsedTimer checks;
        checks.start();
        int selected = 0;
        for (int pass = 0; pass < 10; ++pass) {
            for (int row = 0; row < model.rowCount(); ++row) {
                selected += model.data(model.index(row), LibraryTreeModel::SelectedRole).toBool();
            }
        }
        return std::pair{checks.nsecsElapsed(), selected};
    };
    const auto [withMany, manySelected] = timeRowChecks();
    QCOMPARE(manySelected, 10 * 10000);
    callWith(harness.get(), "clickRow", 5, int(Qt::NoModifier));
    QCOMPARE(model.selectionCount(), 1);
    QVERIFY(QMetaObject::invokeMethod(harness.get(), "rowSelectedInView",
                                      Q_RETURN_ARG(QVariant, shown), Q_ARG(QVariant, 3)));
    QCOMPARE(shown.toBool(), false);
    const auto [withOne, oneSelected] = timeRowChecks();
    QCOMPARE(oneSelected, 10);
    QVERIFY2(withMany < withOne * 4 + 100'000'000,
             qPrintable(QStringLiteral("%1 ns with 10,000 selected, %2 ns with one")
                            .arg(withMany)
                            .arg(withOne)));
}

void LibraryQueueSmokeTest::librarySelectionFollowsItsRowsThroughTreeUpdates() {
    const QString root = QStringLiteral("/music");
    LibraryTreeModel model;
    model.setExpandedKeys(expandedArtistAlbumKeys(root, artistRange(0, 20)));
    QVERIFY(applyLibraryTree(model, encodeRows(artistAlbumRows(root, artistRange(0, 9), 3, plainTrackTitle))));
    const QString removedKey = QStringLiteral("track|/music/Artist 0002/Album/01.flac");
    const QString movedKey = QStringLiteral("track|/music/Artist 0005/Album/02.flac");
    const QString keptKey = QStringLiteral("album|/music|Artist 0007|Album");
    model.selectRow(model.indexForSelectionKey(removedKey));
    model.toggleRowSelected(model.indexForSelectionKey(keptKey));
    model.toggleRowSelected(model.indexForSelectionKey(movedKey));
    QCOMPARE(model.selectionCount(), 3);

    // A new artist comes first, artist 2 goes, and artist 5's tracks are
    // in the opposite order, so its second track moves.
    QVector<BinaryTreeRow> rows = artistAlbumRows(root, {15}, 3, plainTrackTitle);
    rows += artistAlbumRows(root, {0, 1, 3, 4, 5, 6, 7, 8, 9}, 3, plainTrackTitle);
    const auto album = std::find_if(rows.begin(), rows.end(), [](const BinaryTreeRow &row) {
        return row.key == QStringLiteral("album|/music|Artist 0005|Album");
    });
    std::reverse(album + 1, album + 4);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy selection(&model, &LibraryTreeModel::selectionChanged);
    QVERIFY(applyLibraryTree(model, encodeRows(rows)));
    QCOMPARE(resets.count(), 0);
    QVERIFY(selection.count() > 0);

    QCOMPARE(model.selectedKeys(), (QStringList{movedKey, keptKey}));
    const int movedRow = model.indexForSelectionKey(movedKey);
    QVERIFY(model.isRowSelected(movedRow));
    QVERIFY(model.data(model.index(movedRow), LibraryTreeModel::SelectedRole).toBool());
    QVERIFY(!model.data(model.index(movedRow - 1), LibraryTreeModel::SelectedRole).toBool());
    QVERIFY(!model.data(model.index(movedRow + 1), LibraryTreeModel::SelectedRole).toBool());
    QVERIFY(model.isRowSelected(model.indexForSelectionKey(keptKey)));
    for (int row = 0; row < 5; ++row) {
        QVERIFY2(!model.isRowSelected(row), qPrintable(model.selectionKeyForRow(row)));
    }
    QCOMPARE(model.anchorRow(), movedRow);
    QCOMPARE(model.firstSelectedRow(), movedRow);
    // A range from the anchor starts at the row's new place.
    model.selectRange(movedRow + 1);
    QCOMPARE(model.selectedKeys(),
             (QStringList{movedKey, QStringLiteral("track|/music/Artist 0005/Album/01.flac")}));

    // Rows a collapse hides leave the selection too.
    model.toggleKey(QStringLiteral("artist|/music|Artist 0005"));
    QCOMPARE(model.selectionCount(), 0);
    QCOMPARE(model.anchorRow(), -1);
}

void LibraryQueueSmokeTest::playingALibrarySelectionSendsOneBatch() {
    const QString a = QStringLiteral("/music/Artist/Album/01.flac");
    const QString b = QStringLiteral("/music/Artist/Album/02.flac");
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{1, 0, -1, 0, 1, QStringLiteral("Artist"),
        QStringLiteral("artist|/music|Artist"), QStringLiteral("Artist"),
        QStringLiteral("/music/Artist"), {}, {}, {}});
    rows.push_back(BinaryTreeRow{2, 1, -1, 0, 2, QStringLiteral("Album"),
        QStringLiteral("album|/music|Artist|Album"), QStringLiteral("Artist"),
        QStringLiteral("/music/Artist/Album"), {}, {}, QStringList{a, b}});
    for (const QString &path : {a, b}) {
        rows.push_back(BinaryTreeRow{4, 2, -1, 1, 0, QFileInfo(path).fileName(),
            QStringLiteral("track|") + path, QStringLiteral("Artist"), path, {}, path,
            QStringList{path}});
    }
    LibraryTreeModel model;
    model.setExpandedKeys(QStringList{QStringLiteral("artist|/music|Artist"),
                                      QStringLiteral("album|/music|Artist|Album")});
    QVERIFY(applyLibraryTree(model, encodeRows(rows)));
    QCOMPARE(model.rowCount(), 4);

    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> harness(createLibraryControllerHarness(engine, model, &errorText));
    QVERIFY2(harness != nullptr, qPrintable(errorText));

    // Selected from the last row up, played as one batch in tree order.
    callWith(harness.get(), "clickRow", 3, int(Qt::NoModifier));
    callWith(harness.get(), "clickRow", 0, int(Qt::ShiftModifier));
    QVERIFY(QMetaObject::invokeMethod(harness.get(), "playSelection"));
    BinaryBridgeCodec::LibraryQueueItem artist;
    artist.kind = BinaryBridgeCodec::LibraryQueueItem::Artist;
    artist.value = QStringLiteral("artist|/music|Artist");
    const BinaryBridgeCodec::LibraryQueueItems all{
        artist, pathsItem({a, b}), pathsItem({a}), pathsItem({b})};
    QCOMPARE(recordedItems(harness.get(), "played"), (QVector<BinaryBridgeCodec::LibraryQueueItems>{all}));
    QVERIFY(QMetaObject::invokeMethod(harness.get(), "appendSelection"));
    QCOMPARE(recordedItems(harness.get(), "appended"), (QVector<BinaryBridgeCodec::LibraryQueueItems>{all}));

    // A selected row acts for the selection; another row for itself.
    callWith(harness.get(), "playRow", 2);
    callWith(harness.get(), "clickRow", 1, int(Qt::ControlModifier));
    callWith(harness.get(), "insertRow", 1, 7);
    QCOMPARE(recordedItems(harness.get(), "played").size(), 2);
    QCOMPARE(recordedItems(harness.get(), "played").at(1), all);
    QCOMPARE(recordedItems(harness.get(), "inserted"),
             (QVector<BinaryBridgeCodec::LibraryQueueItems>{{pathsItem({a, b})}}));
    QCOMPARE(harness->property("insertedRows").toList(), (QVariantList{7}));
}

void LibraryQueueSmokeTest::revealKeyExpandsADeepKeyInOneCall() {
    // root > artist > album > disc section > track, all collapsed below
    // the root.
    const QString track = QStringLiteral("/music/Artist/Album/CD 2/01.flac");
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{0, 0, -1, 0, 1, QStringLiteral("/music"),
        QStringLiteral("root|/music"), {}, QStringLiteral("/music"), {}, {}, {}});
    rows.push_back(BinaryTreeRow{1, 1, -1, 0, 1, QStringLiteral("Artist"),
        QStringLiteral("artist|/music|Artist"), QStringLiteral("Artist"),
        QStringLiteral("/music/Artist"), {}, {}, {}});
    rows.push_back(BinaryTreeRow{2, 2, -1, 0, 1, QStringLiteral("Album"),
        QStringLiteral("album|/music|Artist|Album"), QStringLiteral("Artist"),
        QStringLiteral("/music/Artist/Album"), {}, {}, QStringList{track}});
    rows.push_back(BinaryTreeRow{3, 3, -1, 0, 1, QStringLiteral("CD 2"),
        QStringLiteral("section|/music|Artist|Album|CD 2"), QStringLiteral("Artist"),
        QStringLiteral("/music/Artist/Album/CD 2"), {}, {}, QStringList{track}});
    rows.push_back(BinaryTreeRow{4, 4, -1, 1, 0, QStringLiteral("01"),
        QStringLiteral("track|") + track, QStringLiteral("Artist"), track, {}, track,
        QStringList{track}});
    LibraryTreeModel model;
    QVERIFY(applyLibraryTree(model, encodeRows(rows)));
    QCOMPARE(model.rowCount(), 2);

    QSignalSpy expansions(&model, &LibraryTreeModel::nodeExpansionRequested);
    QSignalSpy finished(&model, &LibraryTreeModel::revealFinished);
    const int row = model.revealKey(
        QStringList{QStringLiteral("root|/music"), QStringLiteral("artist|/music|Artist"),
                    QStringLiteral("album|/music|Artist|Album"),
                    QStringLiteral("section|/music|Artist|Album|CD 2")},
        QStringLiteral("track|") + track);
    QCOMPARE(row, 4);
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(model.selectionKeyForRow(row), QStringLiteral("track|") + track);
    QVERIFY(!model.revealPending());
    QCOMPARE(finished.count(), 0);
    // The backend hears of each expanded branch; the root is always whole.
    QCOMPARE(expansions.count(), 3);
    // A key the tree lacks is not waited for.
    QCOMPARE(model.revealKey({QStringLiteral("artist|/music|Artist")},
                             QStringLiteral("track|/music/missing.flac")),
             -1);
    QVERIFY(!model.revealPending());
}

void LibraryQueueSmokeTest::revealKeyWaitsForRowsTheBackendHasNotSentWithoutTimers() {
    // The artist's rows come once the backend knows it is expanded.
    const QString lazyArtist = QStringLiteral("artist|/music|Artist A");
    const QString trackKey = QStringLiteral("track|/music/Artist A/Album/02.flac");
    LibraryTreeModel model;
    QVERIFY(applyLibraryTree(model, artistOnlyLazyBinary()));
    QSignalSpy expansions(&model, &LibraryTreeModel::nodeExpansionRequested);
    QSignalSpy finished(&model, &LibraryTreeModel::revealFinished);
    const QStringList expandKeys{lazyArtist, QStringLiteral("album|/music|Artist A|Album")};
    QCOMPARE(model.revealKey(expandKeys, trackKey), -1);
    QVERIFY(model.revealPending());
    QCOMPARE(expansions.count(), 2);

    // A tree from before the expansion still lacks the rows: it waits.
    QVERIFY(applyLibraryTree(model, artistOnlyLazyBinary()));
    QCOMPARE(finished.count(), 0);
    QVERIFY(model.revealPending());

    // The tree with the artist's rows settles it as it is applied.
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{1, 0, -1, 0, 1, QStringLiteral("Artist A (2)"), lazyArtist,
        QStringLiteral("Artist A"), QStringLiteral("/music/Artist A"), {}, {}, {}});
    rows.push_back(BinaryTreeRow{2, 1, -1, 0, 2, QStringLiteral("Album"),
        QStringLiteral("album|/music|Artist A|Album"), QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A/Album"), {}, {}, {}});
    for (int number = 1; number <= 2; ++number) {
        const QString path = QStringLiteral("/music/Artist A/Album/%1.flac")
                                 .arg(number, 2, 10, QLatin1Char('0'));
        rows.push_back(BinaryTreeRow{4, 2, -1, static_cast<quint32>(number), 0,
            QStringLiteral("Track"), QStringLiteral("track|") + path,
            QStringLiteral("Artist A"), path, {}, path, QStringList{path}});
    }
    model.setLibraryTreeFromBinary(encodeRows(rows));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(0).toString(), trackKey);
    QCOMPARE(finished.at(0).at(1).toInt(), model.indexForSelectionKey(trackKey));
    QCOMPARE(finished.at(0).at(1).toInt(), 3);
    QVERIFY(!model.revealPending());

    // Once the rows are there and the key is not, the wait ends empty.
    model.collapseAll();
    QCOMPARE(model.revealKey(expandKeys, QStringLiteral("track|/music/gone.flac")), -1);
    QVERIFY(!model.revealPending());

    // A wait the user overrides is dropped.
    LibraryTreeModel lazy;
    QVERIFY(applyLibraryTree(lazy, artistOnlyLazyBinary()));
    QSignalSpy lazyFinished(&lazy, &LibraryTreeModel::revealFinished);
    QCOMPARE(lazy.revealKey({lazyArtist}, trackKey), -1);
    lazy.cancelReveal();
    QVERIFY(!lazy.revealPending());
    lazy.setLibraryTreeFromBinary(encodeRows(rows));
    QCOMPARE(lazyFinished.count(), 0);
}

void LibraryQueueSmokeTest::queueMovesAndDropsActOnEveryHighlightedRow() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers
Item {
    id: harness
    property var commands: []
    property alias controller: controller
    property var libraryInserts: []
    QtObject {
        id: bridge
        property var queueRows: null
        property int queueLength: 8
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        function record(command) { harness.commands = harness.commands.concat([command]) }
        function selectQueueIndex(index) { selectedQueueIndex = index }
        function moveQueueRows(rows, before) { record("move " + rows.join(",") + " before " + before) }
        function shiftQueueRows(rows, direction) { record("shift " + rows.join(",") + " by " + direction) }
        function insertPaths(paths, row) { record("insert " + paths.join(",") + " at " + row) }
        function queuePathAt(index) { return "" }
    }
    QtObject {
        id: libraryController
        function insertRowItems(rowMap, row) {
            harness.libraryInserts = harness.libraryInserts.concat([rowMap.selectionKey + "@" + row])
        }
    }
    Controllers.QueueController {
        id: controller
        uiBridge: bridge
        tagEditorApi: null
        openTagEditorDialog: function() {}
        autoCenterSelection: false
    }
    function select(rows) { controller.setSelectedIndices(rows) }
    function dropQueueRows(from, at) { controller.dropQueueRows(from, at) }
    function dropLibraryRow(key, at) {
        controller.dropLibraryRow(libraryController, { selectionKey: key }, at)
    }
    function takeCommands() {
        const taken = harness.commands.join("; ")
        harness.commands = []
        return taken
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *controller = root->property("controller").value<QObject *>();
    QVERIFY(controller != nullptr);
    const auto takeCommands = [&root]() {
        QVariant taken;
        QMetaObject::invokeMethod(root.data(), "takeCommands", Q_RETURN_ARG(QVariant, taken));
        return taken.toString();
    };
    const auto select = [&root](const QVariantList &rows) {
        QVERIFY(QMetaObject::invokeMethod(root.data(), "select", Q_ARG(QVariant, QVariant(rows))));
    };
    const auto drop = [&root](int from, int at) {
        QVERIFY(QMetaObject::invokeMethod(root.data(), "dropQueueRows",
            Q_ARG(QVariant, from), Q_ARG(QVariant, at)));
    };

    // Move up and down take every highlighted row, contiguous or not.
    select({2, 3});
    QVERIFY(QMetaObject::invokeMethod(controller, "moveSelected", Q_ARG(QVariant, -1)));
    select({1, 4, 6});
    QVERIFY(QMetaObject::invokeMethod(controller, "moveSelected", Q_ARG(QVariant, 1)));
    // The context menu moves the selection it was opened on, or its row.
    QVERIFY(QMetaObject::invokeMethod(controller, "moveRowsOf", Q_ARG(QVariant, 4),
        Q_ARG(QVariant, -1)));
    QVERIFY(QMetaObject::invokeMethod(controller, "moveRowsOf", Q_ARG(QVariant, 5),
        Q_ARG(QVariant, 1)));
    QCOMPARE(takeCommands(), QStringLiteral(
        "shift 2,3 by -1; shift 1,4,6 by 1; shift 1,4,6 by -1; shift 5 by 1"));

    // A drag of a highlighted row moves all of them to the drop line:
    // before, after and between them.
    select({2, 3, 6});
    drop(3, 0);
    drop(2, 8);
    drop(6, 5);
    drop(6, 3);
    // A drag of another row moves that row alone.
    drop(4, 1);
    QCOMPARE(takeCommands(), QStringLiteral(
        "move 2,3,6 before 0; move 2,3,6 before 8; move 2,3,6 before 5; "
        "move 2,3,6 before 3; move 4 before 1"));

    // Library rows and files go in at the drop line too.
    QVERIFY(QMetaObject::invokeMethod(root.data(), "dropLibraryRow",
        Q_ARG(QVariant, QStringLiteral("album|/music|A|B")), Q_ARG(QVariant, 2)));
    QCOMPARE(root->property("libraryInserts").toStringList(),
             QStringList{QStringLiteral("album|/music|A|B@2")});
    QVERIFY(QMetaObject::invokeMethod(controller, "dropExternalPaths",
        Q_ARG(QVariant, QVariant(QStringList{QStringLiteral("/dropped.flac")})),
        Q_ARG(QVariant, 5)));
    QCOMPARE(takeCommands(), QStringLiteral("insert /dropped.flac at 5"));

    // The drop line is between the rows the drop is nearest.
    const auto insertIndex = [controller](double contentY) {
        QVariant index;
        QMetaObject::invokeMethod(controller, "dropInsertIndex", Q_RETURN_ARG(QVariant, index),
            Q_ARG(QVariant, contentY), Q_ARG(QVariant, 20.0));
        return index.toInt();
    };
    QCOMPARE(insertIndex(0.0), 0);
    QCOMPARE(insertIndex(9.0), 0);
    QCOMPARE(insertIndex(11.0), 1);
    QCOMPARE(insertIndex(75.0), 4);
    QCOMPARE(insertIndex(5000.0), 8);
    QCOMPARE(insertIndex(-40.0), 0);
}

void LibraryQueueSmokeTest::queueSelectionFollowsRowRangeMoves() {
    QueueRowsModel rows;
    QVector<QueueRowData> initial;
    for (int number = 0; number < 8; ++number) {
        QueueRowData row;
        row.title = QStringLiteral("Track %1").arg(number);
        row.path = QStringLiteral("/music/%1.flac").arg(number);
        initial.push_back(row);
    }
    rows.setRows(initial);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("queueRowsModel"), &rows);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers
Item {
    property alias controller: controller
    QtObject {
        id: bridge
        property var queueRows: queueRowsModel
        property int queueLength: 8
        property int selectedQueueIndex: 2
        property int playingQueueIndex: -1
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        function selectQueueIndex(index) { selectedQueueIndex = index }
        function queuePathAt(index) { return "" }
    }
    Controllers.QueueController {
        id: controller
        uiBridge: bridge
        tagEditorApi: null
        openTagEditorDialog: function() {}
        autoCenterSelection: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *controller = root->property("controller").value<QObject *>();
    QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
        Q_ARG(QVariant, QVariant(QVariantList{2, 3, 6}))));
    controller->setProperty("selectionAnchorIndex", 6);
    const auto selection = [controller]() {
        QList<int> indices;
        for (const QVariant &value : controller->property("selectedIndices").toList()) {
            indices.push_back(value.toInt());
        }
        return indices;
    };

    // The backend's row moves for gathering 2, 3 and 6 before row 1: the
    // unmoved rows between them move down past them.
    QVERIFY(rows.moveRows(2, 2, 1));
    QVERIFY(rows.moveRows(6, 1, 3));
    QCOMPARE(rows.rowAt(1)->title, QStringLiteral("Track 2"));
    QCOMPARE(rows.rowAt(3)->title, QStringLiteral("Track 6"));
    QCOMPARE(selection(), (QList<int>{1, 2, 3}));
    QCOMPARE(controller->property("selectionAnchorIndex").toInt(), 3);
    // Rows moved down past the selection leave it in place.
    QVERIFY(rows.moveRows(0, 1, 7));
    QCOMPARE(selection(), (QList<int>{0, 1, 2}));
}

void LibraryQueueSmokeTest::emptyLibraryAndFileMenuAddALibraryFolder() {
    // The bridge runs with the test's own data directories; see main().
    QTemporaryDir music;
    QVERIFY(music.isValid());
    QVERIFY(QDir(music.path()).mkpath(QStringLiteral("First")));
    QVERIFY(QDir(music.path()).mkpath(QStringLiteral("Second")));
    const QString first = QDir(music.path()).filePath(QStringLiteral("First"));
    const QString second = QDir(music.path()).filePath(QStringLiteral("Second"));

    BridgeClient bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(bridge.property("libraryRootCount").toInt(), 0);

    QObject *folderDialog = window->findChild<QObject *>(QStringLiteral("scanFolderDialog"));
    QObject *nameDialog = window->findChild<QObject *>(QStringLiteral("libraryRootNameDialog"));
    QVERIFY(folderDialog != nullptr && nameDialog != nullptr);
    // Picks `folder` in the folder dialog the action opened, then keeps the
    // name the root gets by default.
    const auto chooseFolder = [&](const QString &folder) {
        QTRY_COMPARE(window->property("pendingFolderDialogContext").toString(),
                     QStringLiteral("add-library-root"));
        QVERIFY(QMetaObject::invokeMethod(window, "takeChosenFolder",
                                          Q_ARG(QVariant, folder)));
        QVERIFY(QMetaObject::invokeMethod(folderDialog, "close"));
        QTRY_VERIFY(nameDialog->property("opened").toBool());
        QCOMPARE(nameDialog->property("pathValue").toString(), folder);
        QVERIFY(QMetaObject::invokeMethod(nameDialog, "accept"));
        QTRY_VERIFY(!nameDialog->property("visible").toBool());
    };

    // The first run offers to add a folder, in the queue: the window's one
    // message. The library shows its header with nothing under it.
    QQuickItem *queuePane = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    QVERIFY(queuePane != nullptr);
    QTRY_VERIFY(shownItemNamed(queuePane, QStringLiteral("firstRunAddFolderButton")) != nullptr);
    QTRY_VERIFY(clickVisibleButton(window, QStringLiteral("firstRunAddFolderButton")));
    QCOMPARE(folderDialog->property("title").toString(), QStringLiteral("Add Library Folder"));
    chooseFolder(first);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.property("libraryRootCount").toInt(), 1, 10000);

    // So does the File menu, beside the item that adds a folder to the queue.
    QObject *action = window->findChild<QObject *>(QStringLiteral("addLibraryFolderAction"));
    QVERIFY(action != nullptr);
    QCOMPARE(action->property("text").toString(), QStringLiteral("Add Library Fol&der…"));
    QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
    chooseFolder(second);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.property("libraryRootCount").toInt(), 2, 10000);
    QStringList roots;
    for (const QVariant &entry : bridge.property("libraryRootEntries").toList()) {
        roots.push_back(entry.toMap().value(QStringLiteral("path")).toString());
    }
    roots.sort();
    QCOMPARE(roots, (QStringList{first, second}));
    bool queueItemFound = false;
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->property("text").toString() == QStringLiteral("Add &Folder to Queue…")) {
            queueItemFound = true;
        }
    }
    QVERIFY(queueItemFound);
    // The offscreen platform says so for the folder dialog's widget window.
    dropCapturedMessages(QStringLiteral("This plugin does not support propagateSizeHints()"));
}

namespace {

// The queue pane with its controller, over `queueRowsModel`, and a bridge
// that records the queue commands.
const QByteArray kQueuePaneHarness = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "controllers" as Controllers
import "panes" as Panes

Window {
    id: harness
    width: 900
    height: 400
    visible: true
    property alias controller: queueController
    property var commands: []

    function takeCommands() {
        const taken = harness.commands.join("; ")
        harness.commands = []
        return taken
    }

    QtObject {
        id: bridge
        objectName: "bridge"
        property var queueRows: queueRowsModel
        property int queueLength: queueRowsModel.rowCount()
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        property bool profileLogsEnabled: false
        signal snapshotChanged()
        signal trackIdentityChanged()
        function record(command) { harness.commands = harness.commands.concat([command]) }
        function selectQueueIndex(index) { selectedQueueIndex = index }
        function queuePathAt(index) { return "" }
        function playAt(index) { record("play " + index) }
        function moveQueueRows(rows, before) { record("move " + rows.join(",") + " before " + before) }
        function shiftQueueRows(rows, direction) {}
        function insertPaths(paths, row) {}
        function removeAt(index) {}
        function removeIndices(indices) {}
        function clearQueue() {}
        function openContainingFolder(path) {}
    }

    Components.UiPalette {
        id: palette
        windowRoot: harness
    }

    Controllers.QueueController {
        id: queueController
        objectName: "queueController"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(selection) { return false } }
        openTagEditorDialog: function() {}
        autoCenterSelection: false
    }

    QtObject {
        id: libraryControllerStub
        function insertRowItems(rowMap, row) {}
        function showTrackInLibrary(path) {}
    }

    // The tracks the playback could not read, as the error history has them.
    QtObject {
        id: errorControllerStub
        objectName: "errorController"
        property var unreadablePaths: ({})
    }

    Panes.QueuePane {
        objectName: "queuePane"
        anchors.fill: parent
        controller: queueController
        uiBridge: bridge
        uiPalette: palette
        errorController: errorControllerStub
        preferredHeight: height
        playlistIndicatorColumnWidth: 22
        playlistOrderColumnWidth: 34
        playlistOrderText: function(index) { return String(index + 1) }
        libraryController: libraryControllerStub
        stepScrollView: function(view, wheel, stepSize, stepsPerWheel) {}
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        droppedExternalPaths: function(drop) { return [] }
    }
}
)QML");

void loadQueueRows(QueueRowsModel &rows, int count) {
    QVector<QueueRowData> initial;
    for (int number = 0; number < count; ++number) {
        QueueRowData data;
        data.title = QStringLiteral("Track %1").arg(number);
        data.path = QStringLiteral("/music/%1.flac").arg(number);
        data.lengthText = QStringLiteral("1:00");
        initial.push_back(data);
    }
    rows.setRows(initial);
}

// The scene position `fraction` of the way down row `row` of `view`.
QPoint queueRowPoint(QQuickItem *view, int row, double fraction) {
    QQuickItem *item = nullptr;
    QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, row));
    if (item == nullptr) {
        return {};
    }
    return item->mapToScene(QPointF(item->width() / 2, item->height() * fraction)).toPoint();
}

// Presses at `from`, moves in small steps to `to`, and releases there, as a
// drag with the mouse does.
// QTest dates each release past the double-click interval, so the next press
// does not make a double click.
void dragWithMouse(QQuickWindow *window, QPoint from, QPoint to) {
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
    const int steps = std::max(4, std::abs(to.y() - from.y()) / 4);
    for (int step = 1; step <= steps; ++step) {
        QTest::mouseMove(window, from + (to - from) * step / steps);
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
}

QList<int> queueSelection(QObject *controller) {
    QList<int> indices;
    for (const QVariant &value : controller->property("selectedIndices").toList()) {
        indices.push_back(value.toInt());
    }
    return indices;
}

} // namespace

void LibraryQueueSmokeTest::queueRowsShowTheirStatesInThePaletteColours() {
    QueueRowsModel rows;
    loadQueueRows(rows, 6);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("queueRowsModel"), &rows);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    // An application window, whose palette and font the palette follows.
    QByteArray source = kQueuePaneHarness;
    source.replace("Window {\n    id: harness", "ApplicationWindow {\n    id: harness");
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, source, baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *controller = root->property("controller").value<QObject *>();
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("bridge"));
    QObject *palette = nullptr;
    for (QObject *object : root->findChildren<QObject *>()) {
        if (object->property("uiSelectionColor").isValid() && object->property("boldFont").isValid()) {
            palette = object;
        }
    }
    QVERIFY(controller != nullptr && bridge != nullptr && palette != nullptr);
    QObject *errors = root->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    QTRY_VERIFY(controller->property("view").value<QObject *>() != nullptr);
    auto *view = qobject_cast<QQuickItem *>(controller->property("view").value<QObject *>());
    QTRY_VERIFY(!queueRowPoint(view, 5, 0.5).isNull());
    const auto row = [view](int index) {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, index));
        return item;
    };
    // The first item named `name` in row `index`, at any depth.
    const auto inRow = [&row](int index, const QString &name) -> QQuickItem * {
        QQuickItem *item = row(index);
        if (item == nullptr) {
            return nullptr;
        }
        QList<QQuickItem *> pending{item};
        while (!pending.isEmpty()) {
            QQuickItem *next = pending.takeFirst();
            if (next->objectName() == name) {
                return next;
            }
            pending.append(next->childItems());
        }
        return nullptr;
    };
    const auto cell = [&inRow](int index, const QString &column) {
        return inRow(index, QStringLiteral("queueCell_") + column);
    };
    const auto glyph = [&inRow](int index) { return inRow(index, QStringLiteral("queueRowState")); };
    const auto glyphName = [&glyph](int index) {
        QQuickItem *item = glyph(index);
        return item != nullptr && item->isVisible() ? item->property("source").toString() : QString();
    };
    // The selection tint and ring of a row, or a transparent colour.
    const auto tint = [&inRow](int index) {
        QQuickItem *item = inRow(index, QStringLiteral("rowBackgroundTint"));
        return item != nullptr && item->isVisible()
            ? item->property("color").value<QColor>().name(QColor::HexArgb)
            : QStringLiteral("#00000000");
    };
    const auto ring = [&inRow](int index) {
        QQuickItem *item = inRow(index, QStringLiteral("rowBackgroundTint"));
        return item != nullptr && item->isVisible()
            && QQmlProperty(item, QStringLiteral("border.width")).read().toReal() > 0;
    };
    // As 8-bit ARGB: the palette mixes its colours at a finer precision.
    const auto color = [](QObject *object) {
        return object->property("color").value<QColor>().name(QColor::HexArgb);
    };
    const auto paletteColor = [palette](const char *name) {
        return palette->property(name).value<QColor>().name(QColor::HexArgb);
    };
    const auto weight = [](QObject *text) { return text->property("font").value<QFont>().weight(); };

    // The rows' data, in the tokens' colours, with no alternating rows.
    QCOMPARE(cell(0, "title")->property("text").toString(), QStringLiteral("Track 0"));
    QCOMPARE(cell(3, "order")->property("text").toString(), QStringLiteral("4"));
    QCOMPARE(cell(3, "length")->property("text").toString(), QStringLiteral("1:00"));
    QCOMPARE(cell(3, "artist")->property("text").toString(), QString());
    QCOMPARE(glyphName(0), QString());
    QCOMPARE(tint(0), QStringLiteral("#00000000"));
    QCOMPARE(tint(1), QStringLiteral("#00000000"));
    QCOMPARE(color(cell(1, "title")), paletteColor("text"));
    QCOMPARE(color(cell(1, "artist")), paletteColor("textColumn"));
    QCOMPARE(color(cell(1, "album")), paletteColor("textColumn"));
    QCOMPARE(color(cell(1, "order")), paletteColor("textTertiary"));
    QCOMPARE(color(cell(1, "length")), paletteColor("textSecondary"));
    QCOMPARE(cell(1, "title")->property("font").value<QFont>().family(),
             palette->property("baseFont").value<QFont>().family());
    QCOMPARE(weight(cell(1, "title")), QFont::Normal);
    // Text sits on whole pixels.
    QCOMPARE(cell(1, "title")->y(), std::round(cell(1, "title")->y()));

    // The playing row: an amber play glyph and a semibold amber title.
    bridge->setProperty("playingQueueIndex", 2);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(glyphName(2), QStringLiteral("media-playback-start"));
    QCOMPARE(color(glyph(2)), paletteColor("now"));
    QCOMPARE(color(cell(2, "title")), paletteColor("now"));
    QCOMPARE(weight(cell(2, "title")), QFont::DemiBold);
    QCOMPARE(glyphName(1), QString());
    QVERIFY(row(2)->property("isCurrentQueueRow").toBool());
    // Paused: the pause glyph; amber stays.
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(glyphName(2), QStringLiteral("media-playback-pause"));
    QCOMPARE(color(glyph(2)), paletteColor("now"));
    QCOMPARE(color(cell(2, "title")), paletteColor("now"));
    // Stopped: a neutral stop mark, and the title as any other.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(glyphName(2), QStringLiteral("media-playback-stop"));
    QCOMPARE(color(glyph(2)), paletteColor("textSecondary"));
    QCOMPARE(color(cell(2, "title")), paletteColor("text"));
    QCOMPARE(weight(cell(2, "title")), QFont::Normal);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));

    // An unreadable file: a neutral warning glyph and a quieter title.
    QVERIFY(errors->setProperty("unreadablePaths",
                                QVariantMap{{QStringLiteral("/music/3.flac"),
                                             QStringLiteral("Cannot read the file")}}));
    QTRY_COMPARE(glyphName(3), QStringLiteral("dialog-warning"));
    QCOMPARE(color(glyph(3)), paletteColor("textSecondary"));
    QCOMPARE(color(cell(3, "title")), paletteColor("textSecondary"));
    QCOMPARE(row(3)->property("unreadableReason").toString(), QStringLiteral("Cannot read the file"));
    QVERIFY(errors->setProperty("unreadablePaths", QVariantMap{}));
    QTRY_COMPARE(glyphName(3), QString());

    // Highlighted rows, the playing one among them: the inactive tint while
    // the queue has no focus, and tertiary text steps up to secondary.
    QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
        Q_ARG(QVariant, QVariant(QVariantList{2, 4}))));
    QTRY_COMPARE(tint(4), paletteColor("selectionInactive"));
    QCOMPARE(tint(2), paletteColor("selectionInactive"));
    QCOMPARE(tint(3), QStringLiteral("#00000000"));
    QCOMPARE(color(cell(4, "order")), paletteColor("textSecondary"));
    QCOMPARE(color(cell(3, "order")), paletteColor("textTertiary"));
    QCOMPARE(color(cell(2, "title")), paletteColor("now"));
    QVERIFY(!ring(4));
    // With focus from the keyboard, in the active window: the active tint,
    // and a ring on the current row only.
    bridge->setProperty("selectedQueueIndex", 4);
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    // The activation may hand the queue the focus, which is not the
    // keyboard's: Tab brings it back.
    view->setFocus(false);
    QTRY_VERIFY(!view->hasActiveFocus());
    view->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(view->hasActiveFocus());
    QTRY_COMPARE(tint(4), paletteColor("selection"));
    QCOMPARE(tint(2), paletteColor("selection"));
    QVERIFY(ring(4));
    QVERIFY(!ring(2));
    QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
        Q_ARG(QVariant, QVariant(QVariantList{4}))));
    QTRY_COMPARE(tint(2), QStringLiteral("#00000000"));

    // A palette change at runtime reaches the rows.
    const QString oldSelection = paletteColor("selection");
    QObject *windowPalette = root->property("palette").value<QObject *>();
    QVERIFY(windowPalette != nullptr);
    QVERIFY(windowPalette->setProperty("base", QColor("#203040")));
    QVERIFY(windowPalette->setProperty("window", QColor("#102030")));
    QTRY_COMPARE(paletteColor("view"), QStringLiteral("#ff203040"));
    QVERIFY(paletteColor("selection") != oldSelection);
    QTRY_COMPARE(tint(4), paletteColor("selection"));
    QCOMPARE(color(root->findChild<QQuickItem *>(QStringLiteral("queuePane"))),
             QStringLiteral("#ff203040"));

    // So does a font change, and the cells stay in their header's columns.
    QFont larger = root->property("font").value<QFont>();
    larger.setPointSizeF((larger.pointSizeF() > 0 ? larger.pointSizeF() : 10.0) * 2);
    QVERIFY(root->setProperty("font", larger));
    QTRY_COMPARE(cell(1, "title")->property("font").value<QFont>().pointSizeF(), larger.pointSizeF());
    QTRY_COMPARE(row(1)->height(), palette->property("rowHeight").toReal());
    QQuickItem *titleCell = cell(1, "title");
    QVERIFY(titleCell->y() >= 0 && titleCell->y() + titleCell->height() <= row(1)->height() + 0.5);
}

void LibraryQueueSmokeTest::draggingAHighlightedQueueRowMovesEveryHighlightedRow() {
    QueueRowsModel rows;
    loadQueueRows(rows, 10);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("queueRowsModel"), &rows);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, kQueuePaneHarness, baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *controller = root->property("controller").value<QObject *>();
    QTRY_VERIFY(controller->property("view").value<QObject *>() != nullptr);
    auto *view = qobject_cast<QQuickItem *>(controller->property("view").value<QObject *>());
    QTRY_VERIFY(!queueRowPoint(view, 9, 0.5).isNull());
    const auto takeCommands = [&root]() {
        QVariant taken;
        QMetaObject::invokeMethod(root.data(), "takeCommands", Q_RETURN_ARG(QVariant, taken));
        return taken.toString();
    };

    // Rows 2, 3 and 6 are highlighted; row 3 is dragged, with no modifier,
    // into the upper half of row 8.
    QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
        Q_ARG(QVariant, QVariant(QVariantList{2, 3, 6}))));
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, queueRowPoint(view, 3, 0.5));
    // The press alone keeps the highlight: it may be the start of a drag.
    QCOMPARE(queueSelection(controller), (QList<int>{2, 3, 6}));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, queueRowPoint(view, 3, 0.5));
    // A click that was not a drag selects the row alone.
    QCOMPARE(queueSelection(controller), (QList<int>{3}));
    QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
        Q_ARG(QVariant, QVariant(QVariantList{2, 3, 6}))));

    dragWithMouse(window, queueRowPoint(view, 3, 0.5), queueRowPoint(view, 8, 0.25));
    QCOMPARE(takeCommands(), QStringLiteral("move 2,3,6 before 8"));
    QCOMPARE(queueSelection(controller), (QList<int>{2, 3, 6}));

    // A row that is not highlighted is dragged alone.
    dragWithMouse(window, queueRowPoint(view, 0, 0.5), queueRowPoint(view, 5, 0.75));
    QCOMPARE(takeCommands(), QStringLiteral("move 0 before 6"));
    QCOMPARE(queueSelection(controller), (QList<int>{0}));
}

void LibraryQueueSmokeTest::queueDropsFollowTheRowsAfterRowsAboveAreRemoved() {
    QueueRowsModel rows;
    loadQueueRows(rows, 60);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("queueRowsModel"), &rows);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, kQueuePaneHarness, baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *controller = root->property("controller").value<QObject *>();
    QTRY_VERIFY(controller->property("view").value<QObject *>() != nullptr);
    auto *view = qobject_cast<QQuickItem *>(controller->property("view").value<QObject *>());
    auto *dropArea = root->findChild<QQuickItem *>(QStringLiteral("playlistDropArea"));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("bridge"));
    QVERIFY(dropArea != nullptr && bridge != nullptr);
    QTRY_VERIFY(view->height() > 0);
    const double rowHeight = view->property("rowHeight").toDouble();

    // Row 20 at the top; then the five rows at the top of the queue go,
    // which leaves row 15 there and the list's origin five rows down.
    QVERIFY(QMetaObject::invokeMethod(view, "positionViewAtIndex", Q_ARG(int, 20),
        Q_ARG(int, 0 /* ListView.Beginning */)));
    QCoreApplication::processEvents();
    QVERIFY(rows.removeRowsAt(0, 5));
    bridge->setProperty("queueLength", 55);
    QCoreApplication::processEvents();
    QVERIFY(QMetaObject::invokeMethod(view, "forceLayout"));
    QVERIFY2(std::abs(view->property("originY").toDouble()) > rowHeight,
             qPrintable(QString::number(view->property("originY").toDouble())));
    const double viewTop = dropArea->mapFromItem(view, QPointF(0, 0)).y();
    QQuickItem *top = nullptr;
    int topIndex = -1;
    QMetaObject::invokeMethod(view, "indexAt", Q_RETURN_ARG(int, topIndex), Q_ARG(qreal, 10.0),
        Q_ARG(qreal, view->property("contentY").toReal() + 1.0));
    QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, top), Q_ARG(int, topIndex));
    QVERIFY(top != nullptr);
    const double topRowY = dropArea->mapFromItem(top, QPointF(0, 0)).y();
    QCOMPARE(rows.rowAt(topIndex)->title, QStringLiteral("Track 20"));

    // A drop in the top row's upper half goes before it, one in the lower
    // half of the row three down after that row, and the line is there.
    const auto dropAt = [&](double y) {
        QVERIFY(QMetaObject::invokeMethod(dropArea, "updateQueueDropIndicator", Q_ARG(QVariant, y)));
    };
    dropAt(topRowY + rowHeight * 0.25);
    QCOMPARE(dropArea->property("queueDropInsertIndex").toInt(), topIndex);
    QVERIFY(std::abs(dropArea->property("queueDropIndicatorY").toDouble()
                     - std::max(viewTop, topRowY)) < 0.5);
    dropAt(topRowY + rowHeight * 2.75);
    QCOMPARE(dropArea->property("queueDropInsertIndex").toInt(), topIndex + 3);
    QVERIFY(std::abs(dropArea->property("queueDropIndicatorY").toDouble()
                     - (topRowY + rowHeight * 3)) < 0.5);
}

namespace {

// The library controller with a saved view state whose selection is in
// artist A, over `testLibraryModel`, with the bridge's saves in `savedKeys`.
const QByteArray kRestoreHarness = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    width: 320
    height: 240
    property alias controllerRef: controller
    property alias savedKeys: bridge.savedKeys

    QtObject {
        id: bridge
        property int libraryTrackCount: 2
        property bool libraryViewStateAvailable: true
        property var libraryExpandedKeys: ["artist|/music|Artist A"]
        property string libraryViewSelectionKey: "track|/music/Artist A/01.flac"
        property string libraryViewAnchorKey: ""
        property real libraryViewAnchorOffset: 0
        property var savedKeys: []
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {
            savedKeys = savedKeys.concat([selectionKey])
        }
        function setLibraryNodeExpanded(key, expanded) {}
    }
    QtObject { id: tagEditorApi }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    ListView {
        id: view
        readonly property real rowHeight: 24
        anchors.fill: parent
        model: testLibraryModel
        delegate: Item { width: view.width; height: view.rowHeight }
        Component.onCompleted: controller.registerView(view)
    }

    function start() { controller.requestTreeApply(1, treeBytes) }
    function searchReveal() {
        controller.requestRevealForSearchRow({ artistKey: "artist|/music|Artist B" })
    }
    function clickRow(row) {
        controller.handleRowSelection(
            row, testLibraryModel.rowDataForRow(row), Qt.LeftButton, Qt.NoModifier)
    }
}
)QML");

// Artist A's rows are not loaded yet; artist B's are.
QVector<BinaryTreeRow> restoreHarnessRows() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{1, 0, -1, 0, 1, QStringLiteral("Artist A (1)"),
        QStringLiteral("artist|/music|Artist A"), QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"), {}, {}, {}});
    rows.push_back(BinaryTreeRow{1, 0, -1, 0, 1, QStringLiteral("Artist B (1)"),
        QStringLiteral("artist|/music|Artist B"), QStringLiteral("Artist B"),
        QStringLiteral("/music/Artist B"), {}, {}, {}});
    const QString trackB = QStringLiteral("/music/Artist B/01.flac");
    rows.push_back(BinaryTreeRow{4, 1, -1, 1, 0, QStringLiteral("01"),
        QStringLiteral("track|") + trackB, QStringLiteral("Artist B"), trackB, {}, trackB,
        QStringList{trackB}});
    return rows;
}

} // namespace

void LibraryQueueSmokeTest::aRevealDuringTheSavedViewRestoreKeepsSavingTheView() {
    const QString trackB = QStringLiteral("/music/Artist B/01.flac");
    const QVector<BinaryTreeRow> rows = restoreHarnessRows();
    LibraryTreeModel model;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    engine.rootContext()->setContextProperty(QStringLiteral("treeBytes"), encodeRows(rows));
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, kRestoreHarness, baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *controller = root->property("controllerRef").value<QObject *>();

    // The saved selection is in artist A, whose rows the backend has not
    // sent, so the restore waits for them.
    QVERIFY(QMetaObject::invokeMethod(root.get(), "start"));
    QVERIFY(controller->property("restoringViewState").toBool());
    QVERIFY(model.revealPending());

    // The user reveals artist B from the search first: the restore ends.
    QVERIFY(QMetaObject::invokeMethod(root.get(), "searchReveal"));
    QCOMPARE(controller->property("selectedSelectionKey").toString(),
             QStringLiteral("artist|/music|Artist B"));
    QVERIFY(!controller->property("restoringViewState").toBool());
    QVERIFY(!model.revealPending());
    QTRY_COMPARE(root->property("savedKeys").toStringList().value(0),
                 QStringLiteral("artist|/music|Artist B"));

    // What the user selects next is saved too.
    const int trackRow = model.indexForSelectionKey(QStringLiteral("track|") + trackB);
    QVERIFY(trackRow > 0);
    QVERIFY(QMetaObject::invokeMethod(root.get(), "clickRow", Q_ARG(QVariant, trackRow)));
    QTRY_COMPARE(root->property("savedKeys").toStringList().last(), QStringLiteral("track|") + trackB);
}

void LibraryQueueSmokeTest::escapeCancelsAQueueRowDrag() {
    QueueRowsModel rows;
    loadQueueRows(rows, 10);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("queueRowsModel"), &rows);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, kQueuePaneHarness, baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *controller = root->property("controller").value<QObject *>();
    QTRY_VERIFY(controller->property("view").value<QObject *>() != nullptr);
    auto *view = qobject_cast<QQuickItem *>(controller->property("view").value<QObject *>());
    auto *indicator = root->findChild<QQuickItem *>(QStringLiteral("queueDropIndicator"));
    QVERIFY(indicator != nullptr);
    QTRY_VERIFY(!queueRowPoint(view, 9, 0.5).isNull());
    const auto takeCommands = [&root]() {
        QVariant taken;
        QMetaObject::invokeMethod(root.data(), "takeCommands", Q_RETURN_ARG(QVariant, taken));
        return taken.toString();
    };
    QVERIFY(QMetaObject::invokeMethod(controller, "setSelectedIndices",
        Q_ARG(QVariant, QVariant(QVariantList{2, 3, 6}))));

    // Highlighted row 3 is dragged over row 8, where the line shows.
    const QPoint from = queueRowPoint(view, 3, 0.5);
    const QPoint over = queueRowPoint(view, 8, 0.25);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= 20; ++step) {
        QTest::mouseMove(window, from + (over - from) * step / 20);
    }
    QTRY_VERIFY(indicator->isVisible());

    // Escape stops the drag: the line goes, and moving on and letting go
    // there moves nothing and keeps the highlight.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!indicator->isVisible());
    QTest::mouseMove(window, over + QPoint(0, 4));
    QVERIFY(!indicator->isVisible());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, over + QPoint(0, 4));
    QCOMPARE(takeCommands(), QString());
    QCOMPARE(queueSelection(controller), (QList<int>{2, 3, 6}));

    // The next drag works as before.
    dragWithMouse(window, queueRowPoint(view, 3, 0.5), queueRowPoint(view, 8, 0.25));
    QCOMPARE(takeCommands(), QStringLiteral("move 2,3,6 before 8"));
}

void LibraryQueueSmokeTest::aRevealBeforeTheFirstTreeWinsOverTheSavedViewRestore() {
    const QString trackB = QStringLiteral("/music/Artist B/01.flac");
    LibraryTreeModel model;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    engine.rootContext()->setContextProperty(QStringLiteral("treeBytes"), encodeRows(restoreHarnessRows()));
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, kRestoreHarness, baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *controller = root->property("controllerRef").value<QObject *>();

    // The user reveals artist B from the search before the first tree, and
    // with it the saved selection in artist A, arrives.
    QVERIFY(QMetaObject::invokeMethod(root.get(), "searchReveal"));
    QVERIFY(model.revealPending());
    QVERIFY(QMetaObject::invokeMethod(root.get(), "start"));
    QCOMPARE(controller->property("selectedSelectionKey").toString(),
             QStringLiteral("artist|/music|Artist B"));
    QVERIFY(!controller->property("restoringViewState").toBool());
    QVERIFY(!model.revealPending());
    QTRY_COMPARE(root->property("savedKeys").toStringList().value(0),
                 QStringLiteral("artist|/music|Artist B"));
    // A later tree does not bring the saved selection back.
    QVERIFY(QMetaObject::invokeMethod(controller, "requestTreeApply", Q_ARG(QVariant, 2),
        Q_ARG(QVariant, encodeRows(restoreHarnessRows()))));
    QCOMPARE(controller->property("selectedSelectionKey").toString(),
             QStringLiteral("artist|/music|Artist B"));

    // What the user selects next is saved.
    const int trackRow = model.indexForSelectionKey(QStringLiteral("track|") + trackB);
    QVERIFY(trackRow > 0);
    QVERIFY(QMetaObject::invokeMethod(root.get(), "clickRow", Q_ARG(QVariant, trackRow)));
    QTRY_COMPARE(root->property("savedKeys").toStringList().last(), QStringLiteral("track|") + trackB);
}

std::unique_ptr<QmlSmokeTest> makeLibraryQueueSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<LibraryQueueSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_library_queue.moc"
