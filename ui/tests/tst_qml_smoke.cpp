#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include "../src/LibraryTreeModel.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformItem.h"

namespace {

struct BinaryTreeRow {
    quint8 rowType{0};
    quint16 depth{0};
    qint32 sourceIndex{-1};
    quint16 trackNumber{0};
    quint16 childCount{0};
    QString title;
    QString key;
    QString artist;
    QString path;
    QString coverPath;
    QString trackPath;
    QStringList playPaths;
};

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8U16(QByteArray &out, const QString &value) {
    QByteArray utf8 = value.toUtf8();
    if (utf8.size() > 65535) {
        utf8.truncate(65535);
    }
    appendLe<quint16>(out, static_cast<quint16>(utf8.size()));
    out.append(utf8);
}

QByteArray encodeRows(const QVector<BinaryTreeRow> &rows) {
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const BinaryTreeRow &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, row.sourceIndex);
        appendLe<quint16>(out, row.trackNumber);
        appendLe<quint16>(out, row.childCount);
        appendUtf8U16(out, row.title);
        appendUtf8U16(out, row.key);
        appendUtf8U16(out, row.artist);
        appendUtf8U16(out, row.path);
        appendUtf8U16(out, row.coverPath);
        appendUtf8U16(out, row.trackPath);
        appendLe<quint16>(out, static_cast<quint16>(row.playPaths.size()));
        for (const QString &playPath : row.playPaths) {
            appendUtf8U16(out, playPath);
        }
    }
    return out;
}

QByteArray sampleArtistAlbumTreeBinary() {
    const QString trackPath = QStringLiteral("/music/artist/album/track01.flac");
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        1,
        QStringLiteral("Artist A (1)"),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        2,
        1,
        0,
        0,
        1,
        QStringLiteral("Album A"),
        QStringLiteral("album|Artist A|Album A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A/Album A"),
        QStringLiteral("/music/Artist A/Album A/cover.jpg"),
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        4,
        2,
        -1,
        1,
        0,
        QStringLiteral("01 - Track 01"),
        QStringLiteral("track|/music/artist/album/track01.flac"),
        QStringLiteral("Artist A"),
        trackPath,
        {},
        trackPath,
        QStringList{trackPath},
    });
    return encodeRows(rows);
}

QByteArray artistWithManyAlbumsBinary(int albumCount) {
    QVector<BinaryTreeRow> rows;
    rows.reserve(1 + albumCount * 2);
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        static_cast<quint16>(albumCount),
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

} // namespace

class QmlSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void loadsMainQmlWithFallbackBridge();
    void libraryTreeStartsCollapsedByDefault();
    void artistExpansionPopulatesInBatches();
    void lazyArtistRowRequestsBackendExpansion();
};

void QmlSmokeTest::loadsMainQmlWithFallbackBridge() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);

    const QString qmlPath = QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml");
    QVERIFY2(QFileInfo::exists(qmlPath), qPrintable(QStringLiteral("QML file missing: %1").arg(qmlPath)));

    const QUrl url = QUrl::fromLocalFile(qmlPath);
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
}

void QmlSmokeTest::libraryTreeStartsCollapsedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());

    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::artistExpansionPopulatesInBatches() {
    LibraryTreeModel model;

    model.setLibraryTreeFromBinary(artistWithManyAlbumsBinary(80));
    QTRY_COMPARE(model.rowCount(), 1);

    model.toggleArtist(QStringLiteral("Artist A"));

    QTRY_COMPARE(model.rowCount(), 81);
}

void QmlSmokeTest::lazyArtistRowRequestsBackendExpansion() {
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

QTEST_MAIN(QmlSmokeTest)

#include "tst_qml_smoke.moc"
